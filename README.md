Libavconv
=====

Libavconv is C library functions like `avconv` command.

 * Easy to use and integrate into C.
 * Zero-copy (no fork)
 * Simple but useful buffer control

### Example

```C
int main(int argc, char **argv) {
	int fd_r, fd_w;
	int turn = 0;

	avconvNewFromArgv(&fd_r, &fd_w, argc, argv);

	for (;;) {
		avconvCmd c;
		avconvRequest req = {};
		int n;
		avconvFrame *frame;

		// read frame
		req.frame = 1;

		// probe every 8 turns
		req.probe = (turn % 8) == 0;

		// free all buffers every 4 turns
		// after free frame->buf no longer invalid
		req.free = (turn % 4) == 0;

		c = avconvMakeCmd(AVCONV_REQUEST, &req, sizeof(req));
		write(fd_w, &c, sizeof(c));

		if (req.probe) {
			avconvProbeInfo *pi;

			for (;;) {
				n = read(fd_r, &c, sizeof(c));
				if (n != sizeof(c))
					goto out;
				if (c.type == AVCONV_PROBE_END)
					break;

				pi = (avconvProbeInfo *)c.buf;
				if (pi->idx == 0)
					fprintf(stderr, "probe i=%d pos=%f dur=%f\n", 
							pi->idx, pi->position, pi->duration);
			}
		}

		if (req.frame) {
			n = read(fd_r, &c, sizeof(c));
			if (n != sizeof(c))
				goto out;

			frame = (avconvFrame *)c.buf;
			fprintf(stderr, "frame size=%d\n", frame->size);
			// handle with frame->buf
			// ...
		}

		turn++;
	}

out:
	fprintf(stderr, "avconv session closed\n");
	close(fd_r);
	close(fd_w);

	sleep(1);

	return 0;
}
```

```bash

# use % instead of filename libavconv to handle frames in code 
$ ./test -i in.mp3 -f s16le -ar 44100 -ac 2 %

...
probe i=0 pos=9.195102 dur=10.031020
frame size=4608
frame size=4608
frame size=4608
frame size=4608
frame size=4608
frame size=4608
frame size=4608
frame size=4608
probe i=0 pos=9.404081 dur=10.031020
frame size=4608
frame size=4608
frame size=4608
frame size=4608
frame size=4608
frame size=4608
frame size=4608
frame size=4608
probe i=0 pos=9.613061 dur=10.031020
frame size=4608
...

avconv session closed
```

