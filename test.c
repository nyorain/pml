// testing the mainloop
// TODO: integrate it with pulseaudio while doing some other (fd + defer) stuff

#define _POSIX_C_SOURCE 201710L
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include "mainloop.h"

bool run = true;
unsigned count = 0;

static void timer_cb(struct ml_timer* timer, const struct timespec* t) {
	printf("timer\n");
	if(++count == 10) {
		run = false;
	}

	struct timespec next;
	clock_gettime(CLOCK_REALTIME, &next);
	next.tv_sec += 1;
	ml_timer_restart(timer, &next);
}

static void fd_cb(struct ml_io* io, enum ml_io_flags revents) {
	assert(revents == ml_io_input);
	char buf[512];
	int size = 511;
	int ret;
	while((ret = read(STDIN_FILENO, buf, size)) == size) {
		buf[size] = '\0';
		printf("Got: %s", buf);
	}

	if(ret < 0) {
		printf("Error: %s (%d)\n", strerror(errno), errno);
		return;
	}

	buf[ret] = '\0';
	printf("Got: %s", buf);
}

int main() {
	struct mainloop* ml = mainloop_create();

	struct timespec time;
	clock_gettime(CLOCK_REALTIME, &time);
	time.tv_sec += 1;
	struct ml_timer* timer = ml_timer_new(ml, &time, &timer_cb);
	(void) timer;

	struct ml_io* io = ml_io_new(ml, STDIN_FILENO, ml_io_input, &fd_cb);
	(void) io;

	while(run) {
		mainloop_iterate(ml);
	}

	mainloop_destroy(ml);
}
