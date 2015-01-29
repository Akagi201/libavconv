
#pragma once

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "queue.h"

typedef struct {
	int type;
	int idx;
	float duration;
	float position;
} avconvProbeInfo;

typedef struct {
	unsigned frame:1;
	unsigned probe:1;
	unsigned free:1;
} avconvRequest;

typedef struct {
	int size;
	void *buf;
} avconvFrame;

typedef union {
	avconvRequest req;
	avconvFrame frame;
	avconvProbeInfo pi;
} avconvCmdUnion;

typedef struct {
	int argc;
	char *argv[128];

	union {
		int fd[2];
		struct {
			int r, w;
		} p;
	} cmd_w, cmd_r;

	unsigned quit:1;

	struct {
		unsigned on:1;
		pthread_mutex_t mu;
		queue_t q;
	} myfree;
} avconvCtx;

typedef struct {
	int type;
	char buf[sizeof(avconvCmdUnion)];
} avconvCmd;

enum {
	AVCONV_NONE,
	AVCONV_PROBE_INFO,
	AVCONV_PROBE_END,

	AVCONV_REQUEST,
	AVCONV_FRAME,
};

avconvCmd avconvMakeCmd(int type, void *buf, int size);
void avconvWriteCmd(int type, void *buf, int size);

void avconvNewFromArgv(int *fd_r, int *fd_w, int argc, char **argv);
void avconvCtxDestroy(avconvCtx *c);

extern __thread avconvCtx *gavctx;

