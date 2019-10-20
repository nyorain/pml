#include "mainloop.h"
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>
#include <sys/poll.h>

// TODO: implement the various caches and optimizations that
// keep track of timeouts etc
// TODO: some of the dead+cleanup mechanisms are probably needed
// to allow destroying sources from inside callback handlers.
// with some additional constraints (e.g. no destruction of other
// event sources from within custom_impl.prepare/get_fds) we
// might also get away with just using safe llist iteration, right?
// the dead+cleanup mechanism sounds better though i guess

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
};

struct ml_defer {
	struct ml_defer* prev;
	struct ml_defer* next;
	struct mainloop* mainloop;
	void* data;
	ml_defer_cb cb;
	ml_defer_destroy_cb destroy_cb;
	bool enabled;
};

struct ml_custom {
	struct ml_custom* prev;
	struct ml_custom* next;
	struct mainloop* mainloop;
	void* data;
	const struct ml_custom_impl* impl;
	unsigned n_fds_last;
};

struct mainloop {
	unsigned n_io;
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
};

static unsigned max(unsigned a, unsigned b) {
	return a > b ? a : b;
}

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

static int64_t timespec_ms_from_now(const struct timespec* t) {
	struct timespec diff;
	clock_gettime(CLOCK_REALTIME, &diff);
	timespec_subtract(&diff, t);
	return -timespec_ms(&diff);
}

// mainloop
void mainloop_prepare(struct mainloop* ml) {
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
		if(!t->enabled) {
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
	for(struct ml_custom* c = ml->custom_list; c; c = c->next) {
		assert(c->impl->prepare);
		c->impl->prepare(c);
		unsigned count = 0;
		struct pollfd* fds = NULL;
		if(!ml->rebuild_fds && n_fds < ml->n_fds) {
			fds = &ml->fds[n_fds];
			count = ml->n_fds - n_fds;
		}

		assert(c->impl->get_fds);
		c->impl->get_fds(c, &count, fds);
		c->n_fds_last = n_fds;
		n_fds += count;
	}

	// rebuild fds if needed
	if(ml->rebuild_fds || n_fds > ml->n_fds) {
		ml->rebuild_fds = false;
		ml->fds = realloc(ml->fds, n_fds * sizeof(*ml->fds));
		ml->n_fds = n_fds;

		unsigned i = 0u;
		for(struct ml_io* io = ml->io_list; io; io = io->next) {
			ml->fds[i].fd = io->fd;
			ml->fds[i].events = io->events;
			++i;
		}

		for(struct ml_custom* c = ml->custom_list; c; c = c->next) {
			unsigned count = c->n_fds_last;
			assert(c->impl->get_fds);
			c->impl->get_fds(c, &count, &ml->fds[i]);
			assert(count == c->n_fds_last &&
				"Custom event source changed number of fds without prepare");
			i += count;
		}
	}
}

int mainloop_poll(struct mainloop* ml) {
	if(ml->n_enabled_defered > 0) {
		return 0;
	}

	do {
		ml->poll_ret = poll(ml->fds, ml->n_io, ml->prepared_timeout);
	} while(ml->poll_ret < 0 && errno == EINTR);

	if(ml->poll_ret < 0) {
		printf("mainloop_poll: %s (%d)\n", strerror(errno), errno);
	}

	return ml->poll_ret;
}

void mainloop_dispatch(struct mainloop* ml) {
	if(ml->n_enabled_defered) {
		for(struct ml_defer* d = ml->defer_list; d; d = d->next) {
			if(d->enabled) {
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
		if(!t->enabled) {
			continue;
		}

		struct timespec diff = t->time;
		timespec_subtract(&diff, &now);
		if(timespec_ms(&diff) <= 0) {
			assert(t->cb);
			t->cb(t, &t->time);
			t->enabled = false;
		}
	}

	// dispatch pollfds
	// only needed if poll returned a value >0 though
	if(ml->poll_ret == 0) {
		return;
	}

	struct pollfd* fd = ml->fds;

	// io events
	for(struct ml_io* io = ml->io_list; io; io = io->next) {
		if(fd->revents) {
			assert(io->cb);
			io->cb(io, map_flags_from_libc(fd->revents));
		}
		++fd;
	}

	// custom events
	for(struct ml_custom* c = ml->custom_list; c; c = c->next) {
		assert(c->impl->dispatch);
		c->impl->dispatch(c, c->n_fds_last, fd);
		fd += c->n_fds_last;
	}
}

int mainloop_iterate(struct mainloop* ml) {
	mainloop_prepare(ml);
	int ret = mainloop_poll(ml);
	if(ret < 0) {
		return ret;
	}

	mainloop_dispatch(ml);
	return 0;
}

// ml_io
struct ml_io* ml_io_new(struct mainloop* mainloop, int fd,
		enum ml_io_flags events, ml_io_cb cb) {
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

	--io->mainloop->n_io;
	io->mainloop->rebuild_fds = true;
	free(io);
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
	struct ml_timer* timer = calloc(1, sizeof(*timer));
	timer->mainloop = ml;
	timer->cb = cb;
	timer->time = *time;
	timer->enabled = true;

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

	if(timer->destroy_cb) {
		timer->destroy_cb(timer);
	}

	// unlink
	if(timer->next) {
		timer->next->prev = timer->prev;
	}
	if(timer->prev) {
		timer->prev->next = timer->next;
	}
	if(timer == timer->mainloop->timer_list) {
		timer->mainloop->timer_list = timer->next;
	}

	free(timer);
}

void ml_timer_set_destroy_db(struct ml_timer* timer, ml_timer_destroy_cb dcb) {
	timer->destroy_cb = dcb;
}
struct mainloop* ml_timer_get_mainloop(struct ml_timer* timer) {
	return timer->mainloop;
}

// ml_defer
struct ml_defer* ml_defer_new(struct mainloop* ml, ml_defer_cb cb) {
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
	}
	if(defer->destroy_cb) {
		defer->destroy_cb(defer);
	}

	// unlink
	if(defer->next) {
		defer->next->prev = defer->prev;
	}
	if(defer->prev) {
		defer->prev->next = defer->next;
	}
	if(defer == defer->mainloop->defer_list) {
		defer->mainloop->defer_list = defer->next;
	}

	free(defer);
}
void ml_defer_set_destroy_db(struct ml_defer* defer, ml_defer_destroy_cb dcb) {
	defer->destroy_cb = dcb;
}
struct mainloop* ml_defer_get_mainloop(struct ml_defer* defer) {
	return defer->mainloop;
}

// ml_custom
struct ml_custom* ml_custom_new(struct mainloop* ml, const struct ml_custom_impl* impl) {
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

	// unlink
	if(custom->prev) {
		custom->prev->next = custom->next;
	}
	if(custom->next) {
		custom->next->prev = custom->prev;
	}
	if(custom == custom->mainloop->custom_list) {
		custom->mainloop->custom_list = custom->next;
	}

	free(custom);
}

struct mainloop* ml_custom_get_mainloop(struct ml_custom* custom) {
	return custom->mainloop;
}
