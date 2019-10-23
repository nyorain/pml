#define _POSIX_C_SOURCE 201710L

#include "mainloop.h"
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>
#include <poll.h>

// TODO: implement the various caches and optimizations that
// keep track of timeouts etc
// TODO: keep track of state (prepared/polled/init) in mainloop
// to assert those functions are used correctly?

// additional features to look into
// - supports other clocks that CLOCK_REALTIME
// - support mainloop waking. Could be implemented manually by user as well
//   though (if needed)

struct ml_io {
	struct ml_io* prev;
	struct ml_io* next;
	struct mainloop* mainloop;
	void* data;
	ml_io_cb cb;
	ml_io_destroy_cb destroy_cb;
	int fd;
	enum ml_io_flags events;
	unsigned fd_id;
	bool dead;
};

struct ml_timer {
	struct ml_timer* prev;
	struct ml_timer* next;
	struct mainloop* mainloop;
	struct timespec time;
	bool enabled;
	void* data;
	ml_timer_cb cb;
	ml_timer_destroy_cb destroy_cb;
	bool dead;
};

struct ml_defer {
	struct ml_defer* prev;
	struct ml_defer* next;
	struct mainloop* mainloop;
	void* data;
	ml_defer_cb cb;
	ml_defer_destroy_cb destroy_cb;
	bool enabled;
	bool dead;
};

struct ml_custom {
	struct ml_custom* prev;
	struct ml_custom* next;
	struct mainloop* mainloop;
	void* data;
	const struct ml_custom_impl* impl;
	unsigned n_fds_last;
	bool dead;
};

struct mainloop {
	unsigned n_io; // only alive ones
	unsigned n_fds;
	struct pollfd* fds;

	struct ml_io* io_list;
	struct ml_timer* timer_list;
	struct ml_defer* defer_list;
	struct ml_custom* custom_list;

	bool rebuild_fds;
	int n_enabled_defered;

	int64_t prepared_timeout;
	int poll_ret;

	int n_dead_custom;
	int n_dead_io;
	int n_dead_timer;
	int n_dead_defer;
};

static unsigned min(unsigned a, unsigned b) {
	return a < b ? a : b;
}

// static unsigned max(unsigned a, unsigned b) {
// 	return a > b ? a : b;
// }

static short map_flags_to_libc(enum ml_io_flags flags) {
    return (short)
        ((flags & ml_io_input ? POLLIN : 0) |
         (flags & ml_io_output ? POLLOUT : 0) |
         (flags & ml_io_hangup ? POLLERR : 0) |
         (flags & ml_io_error ? POLLHUP : 0));
}

static enum ml_io_flags map_flags_from_libc(short flags) {
    return
        (flags & POLLIN ? ml_io_input : 0) |
        (flags & POLLOUT ? ml_io_output : 0) |
        (flags & POLLERR ? ml_io_error : 0) |
        (flags & POLLHUP ? ml_io_hangup : 0);
}

static int64_t timespec_ms(const struct timespec* t) {
	return 1000 * t->tv_sec + t->tv_nsec / (1000 * 1000);
}

static void timespec_subtract(struct timespec* a, const struct timespec* minus) {
	a->tv_nsec -= minus->tv_nsec;
	a->tv_sec -= minus->tv_sec;
}

// static int64_t timespec_ms_from_now(const struct timespec* t) {
// 	struct timespec diff;
// 	clock_gettime(CLOCK_REALTIME, &diff);
// 	timespec_subtract(&diff, t);
// 	return -timespec_ms(&diff);
// }

static void cleanup_io(struct mainloop* ml) {
	for(struct ml_io* io = ml->io_list; io && ml->n_dead_io > 0;) {
		if(!io->dead) {
			io = io->next;
			continue;
		}

		if(io->destroy_cb) {
			io->destroy_cb(io);
		}
		if(io->next) {
			io->next->prev = io->prev;
		}
		if(io->prev) {
			io->prev->next = io->next;
		}
		if(io == io->mainloop->io_list) {
			io->mainloop->io_list = io->next;
		}

		--ml->n_dead_io;
		struct ml_io* next = io->next;
		free(io);
		io = next;
	}

	assert(ml->n_dead_io == 0);
}

static void cleanup_timer(struct mainloop* ml) {
	for(struct ml_timer* t = ml->timer_list; t && ml->n_dead_timer > 0;) {
		if(!t->dead) {
			t = t->next;
			continue;
		}

		if(t->destroy_cb) {
			t->destroy_cb(t);
		}
		if(t->next) {
			t->next->prev = t->prev;
		}
		if(t->prev) {
			t->prev->next = t->next;
		}
		if(t == t->mainloop->timer_list) {
			t->mainloop->timer_list = t->next;
		}

		--ml->n_dead_timer;
		struct ml_timer* next = t->next;
		free(t);
		t = next;
	}

	assert(ml->n_dead_timer == 0);
}

static void cleanup_defer(struct mainloop* ml) {
	for(struct ml_defer* d = ml->defer_list; d && ml->n_dead_defer > 0;) {
		if(!d->dead) {
			d = d->next;
			continue;
		}

		if(d->destroy_cb) {
			d->destroy_cb(d);
		}
		if(d->next) {
			d->next->prev = d->prev;
		}
		if(d->prev) {
			d->prev->next = d->next;
		}
		if(d == d->mainloop->defer_list) {
			d->mainloop->defer_list = d->next;
		}

		--ml->n_dead_defer;
		struct ml_defer* next = d->next;
		free(d);
		d = next;
	}

	assert(ml->n_dead_defer == 0);
}

static void cleanup_custom(struct mainloop* ml) {
	for(struct ml_custom* c = ml->custom_list; c && ml->n_dead_custom > 0;) {
		if(!c->dead) {
			c = c->next;
			continue;
		}

		if(c->impl->destroy) {
			c->impl->destroy(c);
		}
		if(c->next) {
			c->next->prev = c->prev;
		}
		if(c->prev) {
			c->prev->next = c->next;
		}
		if(c == c->mainloop->custom_list) {
			c->mainloop->custom_list = c->next;
		}

		--ml->n_dead_custom;
		struct ml_custom* next = c->next;
		free(c);
		c = next;
	}

	assert(ml->n_dead_custom == 0);
}

// mainloop
struct mainloop* mainloop_new(void) {
	struct mainloop* ml = calloc(1, sizeof(*ml));
	return ml;
}

void mainloop_destroy(struct mainloop* ml) {
	if(!ml) {
		return;
	}

	// free all sources
	for(struct ml_custom* c = ml->custom_list; c;) {
		if(c->impl->destroy) {
			c->impl->destroy(c);
		}
		struct ml_custom* n = c->next;
		free(c);
		c = n;
	}
	for(struct ml_io* c = ml->io_list; c;) {
		if(c->destroy_cb) {
			c->destroy_cb(c);
		}
		struct ml_io* n = c->next;
		free(c);
		c = n;
	}
	for(struct ml_defer* c = ml->defer_list; c;) {
		if(c->destroy_cb) {
			c->destroy_cb(c);
		}
		struct ml_defer* n = c->next;
		free(c);
		c = n;
	}
	for(struct ml_timer* c = ml->timer_list; c;) {
		if(c->destroy_cb) {
			c->destroy_cb(c);
		}
		struct ml_timer* n = c->next;
		free(c);
		c = n;
	}

	free(ml);
}

void mainloop_prepare(struct mainloop* ml) {
	// cleanup dead sources
	cleanup_io(ml);
	cleanup_timer(ml);
	cleanup_defer(ml);
	cleanup_custom(ml);

	// if there are enabled defer sources, we will simply dispatch
	// those and not care about the rest
	if(ml->n_enabled_defered) {
		return;
	}

	// check timeout
	ml->prepared_timeout = -1;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	for(struct ml_timer* t = ml->timer_list; t; t = t->next) {
		if(t->dead || !t->enabled) {
			continue;
		}

		struct timespec diff = t->time;
		timespec_subtract(&diff, &now);
		int64_t ms = timespec_ms(&diff);
		if(ms < 0) {
			ml->prepared_timeout = 0;
		} else if(ml->prepared_timeout == -1 || ms < ml->prepared_timeout) {
			ml->prepared_timeout = ms;
		}
	}

	// prepare custom sources
	unsigned n_fds = ml->n_io;
	int timeout;
	for(struct ml_custom* c = ml->custom_list; c; c = c->next) {
		if(c->dead) {
			continue;
		}

		if(c->impl->prepare) {
			c->impl->prepare(c);
		}

		unsigned count = 0;
		struct pollfd* fds = NULL;
		if(!ml->rebuild_fds && n_fds < ml->n_fds) {
			fds = &ml->fds[n_fds];
			count = ml->n_fds - n_fds;
		}

		count = c->impl->query(c, fds, count, &timeout);
		c->n_fds_last = count;
		n_fds += count;
	}

	// rebuild fds if needed
	if(ml->rebuild_fds || n_fds > ml->n_fds) {
		ml->rebuild_fds = false;
		ml->fds = realloc(ml->fds, n_fds * sizeof(*ml->fds));
		ml->n_fds = n_fds;

		unsigned i = 0u;
		for(struct ml_io* io = ml->io_list; io; io = io->next) {
			if(io->dead) {
				continue;
			}

			ml->fds[i].fd = io->fd;
			ml->fds[i].events = map_flags_to_libc(io->events);
			++i;
		}

		for(struct ml_custom* c = ml->custom_list; c; c = c->next) {
			if(c->dead) {
				continue;
			}

			unsigned count = c->n_fds_last;
			count = c->impl->query(c, &ml->fds[i], count, &timeout);
			assert(count == c->n_fds_last &&
				"Custom event source changed number of fds without prepare");
			if(timeout >= 0 && timeout < ml->prepared_timeout) {
				ml->prepared_timeout = timeout;
			}
			i += count;
		}
	}

	return;
}

int mainloop_poll(struct mainloop* ml) {
	if(ml->n_enabled_defered > 0) {
		return 0;
	}

	do {
		ml->poll_ret = poll(ml->fds, ml->n_fds, ml->prepared_timeout);
	} while(ml->poll_ret < 0 && errno == EINTR);

	if(ml->poll_ret < 0) {
		printf("mainloop_poll: %s (%d)\n", strerror(errno), errno);
	}

	return ml->poll_ret;
}

void mainloop_dispatch(struct mainloop* ml, struct pollfd* fds, unsigned n_fds) {
	if(ml->n_enabled_defered) {
		for(struct ml_defer* d = ml->defer_list; d; d = d->next) {
			if(d->enabled && !d->dead) {
				assert(d->cb);
				d->cb(d);
			}
		}

		// if there were any deferred events enabled, we didn't ever
		// poll or wait for any timeout and therefore don't have
		// to dispatch any other sources
		return;
	}

	// dispatch time events
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	for(struct ml_timer* t = ml->timer_list; t; t = t->next) {
		if(!t->enabled || t->dead) {
			continue;
		}

		struct timespec diff = t->time;
		timespec_subtract(&diff, &now);
		if(timespec_ms(&diff) <= 0) {
			assert(t->cb);
			t->enabled = false;
			t->cb(t, &t->time);
		}
	}

	// dispatch pollfds
	// only needed if poll returned a value >0 though
	if(ml->poll_ret == 0) {
		return;
	}

	struct pollfd* fd = fds;

	// io events
	for(struct ml_io* io = ml->io_list; io; io = io->next) {
		if(io->dead) {
			continue;
		}

		if(fd->revents) {
			assert(io->cb);
			io->cb(io, map_flags_from_libc(fd->revents));
		}
		++fd;
		assert(fd - fds <= n_fds);
	}

	// custom events
	for(struct ml_custom* c = ml->custom_list; c; c = c->next) {
		if(c->dead) {
			continue;
		}

		c->impl->dispatch(c, fd, c->n_fds_last);
		fd += c->n_fds_last;
		assert(fd - fds <= n_fds);
	}
}

int mainloop_iterate(struct mainloop* ml) {
	mainloop_prepare(ml);
	int ret = mainloop_poll(ml);
	if(ret < 0) {
		return ret;
	}

	mainloop_dispatch(ml, ml->fds, ml->n_fds);
	return 0;
}

unsigned mainloop_query(struct mainloop* ml, struct pollfd* fds, unsigned n_fds,
		int* timeout) {
	unsigned size = min(n_fds, ml->n_fds) * sizeof(*fds);
	memcpy(fds, ml->fds, size);
	*timeout = ml->prepared_timeout;
	return ml->n_fds;
}

// ml_io
struct ml_io* ml_io_new(struct mainloop* mainloop, int fd,
		enum ml_io_flags events, ml_io_cb cb) {
	assert(mainloop);
	assert(fd >= 0);
	assert(cb);

	struct ml_io* io = calloc(1, sizeof(*io));
	io->mainloop = mainloop;
	io->fd = fd;
	io->events = events;
	io->cb = cb;
	io->fd_id = UINT_MAX;

	mainloop->rebuild_fds = true;
	++mainloop->n_io;

	if(mainloop->io_list) {
		mainloop->io_list->prev = io;
	}
	io->next = mainloop->io_list;
	mainloop->io_list = io;

	return io;
}

void ml_io_set_data(struct ml_io* io, void* data) {
	io->data = data;
}

void* ml_io_get_data(struct ml_io* io) {
	return io->data;
}

int ml_io_get_fd(struct ml_io* io) {
	return io->fd;
}

void ml_io_destroy(struct ml_io* io) {
	if(!io) {
		return;
	}

	io->dead = true;
	--io->mainloop->n_io;
	++io->mainloop->n_dead_io;
	io->mainloop->rebuild_fds = true;
}

void ml_io_set_destroy_db(struct ml_io* io, ml_io_destroy_cb dcb) {
	io->destroy_cb = dcb;
}

void ml_io_events(struct ml_io* io, enum ml_io_flags events) {
	io->events = events;
	if(io->fd_id != UINT_MAX && !io->mainloop->rebuild_fds) {
		io->mainloop->fds[io->fd_id].events = events;
	}
}

struct mainloop* ml_io_get_mainloop(struct ml_io* io) {
	return io->mainloop;
}

// ml_timer
struct ml_timer* ml_timer_new(struct mainloop* ml, const struct timespec* time,
		ml_timer_cb cb) {
	assert(ml);
	assert(cb);

	struct ml_timer* timer = calloc(1, sizeof(*timer));
	timer->mainloop = ml;
	timer->cb = cb;
	timer->enabled = time;
	if(time) {
		timer->time = *time;
	}

	if(ml->timer_list) {
		ml->timer_list->prev = timer;
	}
	timer->next = ml->timer_list;
	ml->timer_list = timer;

	return timer;
}

void ml_timer_restart(struct ml_timer* timer, const struct timespec* time) {
	timer->enabled = time;
	if(time) {
		timer->time = *time;
	}
}

void ml_timer_set_data(struct ml_timer* timer, void* data) {
	timer->data = data;
}

void* ml_timer_get_data(struct ml_timer* timer) {
	return timer->data;
}

void ml_timer_destroy(struct ml_timer* timer) {
	if(!timer) {
		return;
	}

	timer->enabled = false;
	timer->dead = true;
	++timer->mainloop->n_dead_timer;
}

void ml_timer_set_destroy_db(struct ml_timer* timer, ml_timer_destroy_cb dcb) {
	timer->destroy_cb = dcb;
}
struct mainloop* ml_timer_get_mainloop(struct ml_timer* timer) {
	return timer->mainloop;
}

// ml_defer
struct ml_defer* ml_defer_new(struct mainloop* ml, ml_defer_cb cb) {
	assert(ml);
	assert(cb);

	struct ml_defer* defer = calloc(1, sizeof(*defer));
	defer->cb = cb;
	defer->mainloop = ml;
	defer->enabled = true;
	++ml->n_enabled_defered;

	if(ml->defer_list) {
		ml->defer_list->prev = defer;
	}
	defer->next = ml->defer_list;
	ml->defer_list = defer;

	return defer;
}

void ml_defer_enable(struct ml_defer* defer, bool enable) {
	if(defer->enabled == enable) {
		return;
	}

	defer->enabled = enable;
	if(defer->enabled) {
		++defer->mainloop->n_enabled_defered;
	} else {
		--defer->mainloop->n_enabled_defered;
	}
}

void ml_defer_set_data(struct ml_defer* defer, void* data) {
	defer->data = data;
}

void* ml_defer_get_data(struct ml_defer* defer) {
	return defer->data;
}

void ml_defer_destroy(struct ml_defer* defer) {
	if(!defer) {
		return;
	}
	if(defer->enabled) {
		--defer->mainloop->n_enabled_defered;
		defer->enabled = false;
	}

	defer->dead = true;
	++defer->mainloop->n_dead_defer;
}
void ml_defer_set_destroy_db(struct ml_defer* defer, ml_defer_destroy_cb dcb) {
	defer->destroy_cb = dcb;
}
struct mainloop* ml_defer_get_mainloop(struct ml_defer* defer) {
	return defer->mainloop;
}

// ml_custom
struct ml_custom* ml_custom_new(struct mainloop* ml, const struct ml_custom_impl* impl) {
	assert(ml);
	assert(impl);
	assert(impl->dispatch);
	assert(impl->query);

	struct ml_custom* custom = calloc(1, sizeof(*custom));
	custom->mainloop = ml;
	custom->impl = impl;

	if(ml->custom_list) {
		ml->custom_list->prev = custom;
	}
	custom->next = ml->custom_list;
	ml->custom_list = custom;

	return custom;
}

void ml_custom_set_data(struct ml_custom* custom, void* data) {
	custom->data = data;
}

void* ml_custom_get_data(struct ml_custom* custom) {
	return custom->data;
}

void ml_custom_destroy(struct ml_custom* custom) {
	if(!custom) {
		return;
	}

	custom->dead = true;
	++custom->mainloop->n_dead_custom;
}

struct mainloop* ml_custom_get_mainloop(struct ml_custom* custom) {
	return custom->mainloop;
}
