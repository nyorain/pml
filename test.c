// testing the mainloop
// TODO: integrate it with pulseaudio while doing some other (fd + defer) stuff

#define _POSIX_C_SOURCE 201710L
#include <stdio.h>
#include <time.h>
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

int main() {
	struct mainloop* ml = mainloop_create();

	struct timespec time;
	clock_gettime(CLOCK_REALTIME, &time);
	time.tv_sec += 1;
	struct ml_timer* timer = ml_timer_new(ml, &time, &timer_cb);
	(void) timer;

	while(run) {
		mainloop_iterate(ml);
	}

	mainloop_destroy(ml);
}
