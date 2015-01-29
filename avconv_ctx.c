
#include "config.h"
#include "avconv.h"
#include "cmdutils.h"
#include "avconv_ctx.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

#if HAVE_MALLOC_H
#include <malloc.h>
#endif

#include "libavformat/avformat.h"
#include "libavformat/url.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavresample/avresample.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/fifo.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"

static inline float time_r2f(int64_t t, AVRational tb) {
	if (t == AV_NOPTS_VALUE)
		return 0.0;
	return (float)t * tb.num / tb.den;
}

typedef struct {
	queue_t q;
	void *p;
} myfreeNode;

static void myfreeAppend(avconvCtx *c, void *p) {
	myfreeNode *n = malloc(sizeof(myfreeNode));
	n->p = p;

	pthread_mutex_lock(&c->myfree.mu);
	queue_insert_tail(&c->myfree.q, &n->q);
	pthread_mutex_unlock(&c->myfree.mu);
}

static void myfreeFreeAll(avconvCtx *c) {
	int nr = 0;

	pthread_mutex_lock(&c->myfree.mu);
	while (!queue_empty(&c->myfree.q)) {
		myfreeNode *n = queue_data(queue_head(&c->myfree.q), myfreeNode, q);
		queue_remove(&n->q);
		free(n->p);
		free(n);
		nr++;
	}
	pthread_mutex_unlock(&c->myfree.mu);

	//fprintf(stderr, "total %d freed\n", nr);
}

static void myfreeInit(avconvCtx *c) {
	c->myfree.on = 1;
	pthread_mutex_init(&c->myfree.mu, NULL);
	queue_init(&c->myfree.q);
}

static void myfreeDestroy(avconvCtx *c) {
	if (c->myfree.on) {
		myfreeFreeAll(c);
		pthread_mutex_destroy(&c->myfree.mu);
	}
}

void avconvCtxDestroy(avconvCtx *c) {
	int i;

	myfreeDestroy(c);

	for (i = 0; i < c->argc; i++)
		free(c->argv[i]);

	close(c->cmd_w.p.r);
	close(c->cmd_r.p.w);

	free(c);
}

typedef struct {
	int size;
} CallbackContext;

static int callback_open(URLContext *h, const char *uri, int flags) {
	CallbackContext *cc = h->priv_data;
	cc->size = 0;
	//fprintf(stderr, "callback.open %s\n", uri);
	return 0;
}

static int callback_read(URLContext *h, uint8_t *buf, int size) {
	//CallbackContext *cc = h->priv_data;
	//fprintf(stderr, "callback.read size=%d\n", size);
	return size;
}

static int callback_write(URLContext *h, const uint8_t *buf, int size) {
	CallbackContext *cc = h->priv_data;

	//fprintf(stderr, "callback.write size=%d\n", size);

	while (!gavctx->quit) {
		avconvCmd c = {};
		avconvRequest *req;
		int n;

		n = read(gavctx->cmd_w.p.r, &c, sizeof(c));
		if (n != sizeof(c)) {
			gavctx->quit = 1;
			return -1;
		}

		req = (avconvRequest *)c.buf;

		if (req->probe) {
			int i;
			for (i = 0; i < nb_input_streams; i++) {
				InputStream *ist = input_streams[i];
				avconvProbeInfo pi = {
					.duration = time_r2f(ist->st->duration, ist->st->time_base),
					.position = time_r2f(ist->last_pts, ist->st->time_base),
					.type = ist->st->codec->codec_type,
					.idx = i,
				};
				avconvWriteCmd(AVCONV_PROBE_INFO, &pi, sizeof(pi));
			}
			avconvWriteCmd(AVCONV_PROBE_END, NULL, 0);
		}

		if (req->free) {
			myfreeFreeAll(gavctx);
		}

		if (req->frame) {
			avconvFrame af = {.buf = buf, .size = size};
			//fprintf(stderr, "frame \n");
			avconvWriteCmd(AVCONV_FRAME, &af, sizeof(af));
			return size;
		}
	}

	return -1;
}

static int callback_close(URLContext *h) {
	//CallbackContext *cc = h->priv_data;
	//fprintf(stderr, "callback.close totsize=%d\n", cc->size);
	return 0;
}

static URLProtocol urlprot_callback = {
	.name = "callback",
	.priv_data_size = sizeof(CallbackContext),
	.url_open = callback_open,
	.url_read = callback_read,
	.url_write = callback_write,
	.url_close = callback_close,
	.flags = URL_PROTOCOL_FLAG_NETWORK,
};

void *my_malloc(size_t size);
void *my_memalign(size_t align, size_t size);
int   my_posix_memalign(void **ptr, size_t align, size_t size);
void *my_realloc(void *ptr, size_t size);
void  my_free(void *ptr);

void *my_malloc(size_t size) {
	//fprintf(stderr, "malloc %ld\n", size);
	return malloc(size);
}

void *my_memalign(size_t align, size_t size) {
	return memalign(align, size);
}

int my_posix_memalign(void **ptr, size_t align, size_t size) {
	return posix_memalign(ptr, align, size);
}

void *my_realloc(void *ptr, size_t size) {
	//fprintf(stderr, "realloc %p %ld\n", ptr, size);
	return realloc(ptr, size);
}

void my_free(void *ptr) {
	//fprintf(stderr, "free %p\n", ptr);

	if (gavctx && gavctx->myfree.on) {
		myfreeAppend(gavctx, ptr);
	} else {
		free(ptr);
	}
}

__thread avconvCtx *gavctx;
__thread jmp_buf program_jmpbuf;

static void *avconvThread(void *_c) {
	int progret;

	gavctx = _c;
	progret = setjmp(program_jmpbuf);
	if (progret) {
		progret -= 255;
		avconv_cleanup(progret);
		return NULL;
	}

	avconv_conv(gavctx->argc, gavctx->argv);
	return NULL;
}

static void log_cb(void *ctx, int level, const char *fmt, va_list args) {
}

void avconvNewFromArgv(int *fd_r, int *fd_w, int argc, char **argv) {
	static int inited;
	avconvCtx *ac;
	int i;
	int psize;
	pthread_t t;

	if (!inited) {
		avcodec_register_all();
	#if CONFIG_AVDEVICE
		avdevice_register_all();
	#endif
		avfilter_register_all();
		av_register_all();
		avformat_network_init();
		ffurl_register_protocol(&urlprot_callback);
		av_log_set_callback(log_cb);

		inited++;
	}

	//av_log_set_flags(AV_LOG_SKIP_REPEATED);
	//parse_loglevel(argc, argv, options);
	
	ac = malloc(sizeof(avconvCtx));
	memset(ac, 0, sizeof(avconvCtx));

	if (pipe(ac->cmd_w.fd) < 0)
		goto errout;
	
	if (pipe(ac->cmd_r.fd) < 0)
		goto errout;

	*fd_r = ac->cmd_r.p.r;
	*fd_w = ac->cmd_w.p.w;
	
	ac->myfree.on = 1;
	if (ac->myfree.on) 
		myfreeInit(ac);

	ac->argc = argc;
	for (i = 0; i < argc; i++)
		ac->argv[i] = strdup(argv[i]);

	pthread_create(&t, NULL, avconvThread, ac);
	pthread_detach(t);

	return;

errout:
	free(ac);
}

avconvCmd avconvMakeCmd(int type, void *buf, int size) {
	avconvCmd c = {.type = type};
	if (buf)
		memcpy(c.buf, buf, size);
	return c;
}

void avconvWriteCmd(int type, void *buf, int size) {
	avconvCmd c = avconvMakeCmd(type, buf, size);
	write(gavctx->cmd_r.p.w, &c, sizeof(c));
}

