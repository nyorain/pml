#include "mainloop.h"
#include <stdio.h>
#include <assert.h>

unsigned count = 0u;

void defer_cb(struct ml_defer* d) {
	printf("nested iteration %d\n", count);
	++count;
	struct mainloop* ml = ml_defer_get_mainloop(d);
	if(count == 2) {
		ml_defer_destroy(d);
	}

	// finish the current iteration
	mainloop_iterate(ml, false);

	// start a new iteration that should trigger the defer source again
	mainloop_iterate(ml, false);
}

int main() {
	struct mainloop* ml = mainloop_new();
	ml_defer_new(ml, defer_cb);

	mainloop_iterate(ml, true);
	mainloop_destroy(ml);
	assert(count == 2);
}

