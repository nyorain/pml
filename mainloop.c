// Copyright 2004-2006 Lennart Poettering
// Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB
// Copyright 2019 Jan Kelling
//
// PulseAudio is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//
// PulseAudio is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.

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

// TODO: rebuilding optimizations. On event source destruction
// we could just set the fds to -1 (and potentially even re-use
// them later on for different fds), instead of rebuilding
// TODO: implement the various caches and optimizations that
// keep track of timeouts etc
// TODO: keep track of state (prepared/polled/init) in mainloop
// to assert those functions are used correctly?
// TODO: remove destruction callbacks? can't really think of a
// useful use case

// additional features to look into
// - supports other clocks that CLOCK_REALTIME
// - support mainloop waking. Could be implemented manually by user as well
//   though (if needed)
// - support less timer delay by preparing a timespec (epoch) instead
//   of already calculating the resulting timeout interval

enum state {
	state_none,
	state_preparing,
	state_prepared,
	state_polled,
	state_dispatch_timer,
	state_dispatch_io,
	state_dispatch_defer,
	state_dispatch_custom,
};

struct ml_io {
	struct ml_io* prev;
	struct ml_io* next;
	struct mainloop* mainloop;
	void* data;
	ml_io_cb cb;
	int fd;
	enum ml_io_flags events;
	unsigned fd_id;
	bool dead;
	unsigned added;
};

struct ml_timer {
	struct ml_timer* prev;
	struct ml_timer* next;
	struct mainloop* mainloop;
	struct timespec time;
	bool enabled;
	void* data;
	ml_timer_cb cb;
	bool dead;
	unsigned added;
};

struct ml_defer {
	struct ml_defer* prev;
	struct ml_defer* next;
	struct mainloop* mainloop;
	void* data;
	ml_defer_cb cb;
	bool enabled;
	bool dead;
	unsigned added;
};

struct ml_custom {
	struct ml_custom* prev;
	struct ml_custom* next;
	struct mainloop* mainloop;
	void* data;
	const struct ml_custom_impl* impl;
	unsigned fds_id;
	unsigned n_fds_last;
	bool dead;
	unsigned added;
};

struct mainloop {
	unsigned n_io; // only alive ones
	unsigned n_fds;
	struct pollfd* fds;

	struct {
		struct ml_io* first;
		struct ml_io* last;
	} io;

	struct {
		struct ml_timer* first;
		struct ml_timer* last;
	} timer;

	struct {
		struct ml_defer* first;
		struct ml_defer* last;
	} defer;

	struct {
		struct ml_custom* first;
		struct ml_custom* last;
	} custom;

	bool rebuild_fds;
	int n_enabled_defered;

	int64_t prepared_timeout;
	int poll_ret;

	enum state state;
	void* state_data;
	unsigned dispatch_depth;

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
         (flags & ml_io_hangup ? POLLHUP : 0) |
         (flags & ml_io_error ? POLLERR : 0));
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

static void destroy_io(struct ml_io* io) {
	if(io->next) io->next->prev = io->prev;
	if(io->prev) io->prev->next = io->next;
	if(io == io->mainloop->io.first) io->mainloop->io.first = io->next;
	if(io == io->mainloop->io.last) io->mainloop->io.last = io->prev;
	free(io);
}

static void destroy_timer(struct ml_timer* t) {
	if(t->next) t->next->prev = t->prev;
	if(t->prev) t->prev->next = t->next;
	if(t == t->mainloop->timer.first) t->mainloop->timer.first = t->next;
	if(t == t->mainloop->timer.last) t->mainloop->timer.last = t->prev;
}

static void destroy_defer(struct ml_defer* d) {
	if(d->next) d->next->prev = d->prev;
	if(d->prev) d->prev->next = d->next;
	if(d == d->mainloop->defer.first) d->mainloop->defer.first = d->next;
	if(d == d->mainloop->defer.last) d->mainloop->defer.last = d->prev;
	free(d);
}

static void destroy_custom(struct ml_custom* c) {
	if(c->next) c->next->prev = c->prev;
	if(c->prev) c->prev->next = c->next;
	if(c == c->mainloop->custom.first) c->mainloop->custom.first = c->next;
	if(c == c->mainloop->custom.last) c->mainloop->custom.last = c->prev;
}

static void cleanup_io(struct mainloop* ml) {
	struct ml_io* io = ml->io.first;
	while(io->next && ml->n_dead_io > 0) {
		if(!io->dead || ml->state_data == io) {
			io = io->next;
			continue;
		}

		--ml->n_dead_io;
		struct ml_io* next = io->next;
		destroy_io(io);
		io = next;
	}

	assert(ml->n_dead_io == 0);
}

static void cleanup_timer(struct mainloop* ml) {
	struct ml_timer* t = ml->timer.first;
	while(t->next && ml->n_dead_timer > 0) {
		if(!t->dead || ml->state_data == t) {
			t = t->next;
			continue;
		}

		--ml->n_dead_timer;
		struct ml_timer* next = t->next;
		destroy_timer(t);
		t = next;
	}

	assert(ml->n_dead_timer == 0);
}

static void cleanup_defer(struct mainloop* ml) {
	struct ml_defer* d = ml->defer.first;
	while(d && ml->n_dead_defer > 0) {
		if(!d->dead || ml->state_data == d) {
			d = d->next;
			continue;
		}

		--ml->n_dead_defer;
		struct ml_defer* next = d->next;
		destroy_defer(d);
		d = next;
	}

	assert(ml->n_dead_defer == 0);
}

static void cleanup_custom(struct mainloop* ml) {
	struct ml_custom* c = ml->custom.first;
	while(c && ml->n_dead_custom > 0) {
		if(!c->dead || ml->state_data == c) {
			c = c->next;
			continue;
		}

		--ml->n_dead_custom;
		struct ml_custom* next = c->next;
		destroy_custom(c);
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

	assert(ml->state == state_none);

	// free all sources
	for(struct ml_custom* c = ml->custom.first; c;) {
		struct ml_custom* n = c->next;
		free(c);
		c = n;
	}
	for(struct ml_io* c = ml->io.first; c;) {
		struct ml_io* n = c->next;
		free(c);
		c = n;
	}
	for(struct ml_defer* c = ml->defer.first; c;) {
		struct ml_defer* n = c->next;
		free(c);
		c = n;
	}
	for(struct ml_timer* c = ml->timer.first; c;) {
		struct ml_timer* n = c->next;
		free(c);
		c = n;
	}

	free(ml);
}

void mainloop_prepare(struct mainloop* ml) {
	assert(ml->state != state_preparing);
	assert(ml->state != state_polled);
	assert(ml->state != state_prepared);

	// cleanup dead sources
	// if we are in the middle of dispatching, this will keep
	// the current source, even if marked dead.
	cleanup_io(ml);
	cleanup_timer(ml);
	cleanup_defer(ml);
	cleanup_custom(ml);

	// dispatching isn't finished yet, just continue it
	if(ml->state != state_none) {
		return;
	}

	ml->state = state_preparing;

	// TODO: caches
	// if there are enabled defer sources, we will simply dispatch
	// those and not care about the rest
	// if(ml->n_enabled_defered) {
	// 	ml->state = state_prepared;
	// 	return;
	// }

	// start with custom sources since they may change stuff
	ml->prepared_timeout = -1;
	unsigned n_fds = ml->n_io;
	for(struct ml_custom* c = ml->custom.first; c; c = c->next) {
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

		int timeout;
		count = c->impl->query(c, fds, count, &timeout);
		assert(timeout >= -1);

		if(timeout != -1 && (ml->prepared_timeout == -1 ||
				timeout < ml->prepared_timeout)) {
			ml->prepared_timeout = timeout;
		}

		c->n_fds_last = count;
		n_fds += count;
	}

	// check timeout
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	for(struct ml_timer* t = ml->timer.first; t; t = t->next) {
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

	// rebuild fds if needed
	if(ml->rebuild_fds || n_fds > ml->n_fds) {
		ml->rebuild_fds = false;
		ml->fds = realloc(ml->fds, n_fds * sizeof(*ml->fds));
		ml->n_fds = n_fds;

		unsigned i = 0u;
		for(struct ml_io* io = ml->io.first; io; io = io->next) {
			if(io->dead) {
				continue;
			}

			ml->fds[i].fd = io->fd;
			ml->fds[i].events = map_flags_to_libc(io->events);
			io->fd_id = i;
			++i;
		}

		for(struct ml_custom* c = ml->custom.first; c; c = c->next) {
			if(c->dead) {
				continue;
			}

			int timeout;
			unsigned count = c->impl->query(c, &ml->fds[i], c->n_fds_last, &timeout);
			assert(count == c->n_fds_last &&
				"Custom event source changed number of fds without prepare");

			c->fds_id = i;
			i += count;
		}
	}

	ml->state = state_prepared;
	return;
}

int mainloop_poll(struct mainloop* ml) {
	assert(ml->state != state_none);
	assert(ml->state != state_preparing);
	assert(ml->state != state_polled);

	// dispatching not finished yet
	if(ml->state != state_prepared) {
		return 0;
	}

	do {
		// printf("timeout: %d\n", (int) ml->prepared_timeout);
		ml->poll_ret = poll(ml->fds, ml->n_fds, ml->prepared_timeout);
	} while(ml->poll_ret < 0 && errno == EINTR);

	if(ml->poll_ret < 0) {
		fprintf(stderr, "mainloop poll: %s (%d)\n", strerror(errno), errno);
		ml->state = state_none;
		return ml->poll_ret;
	}

	ml->state = state_polled;
	return ml->poll_ret;
}

static bool dispatch_defer(struct mainloop* ml) {
	struct ml_defer* d = ml->defer.first;
	if(ml->state == state_dispatch_defer) {
		d = ml->state_data;
		assert(d);
		d = d->next;
	}

	ml->state = state_dispatch_defer;
	for(; d; d = d->next) {
		if(d->enabled && !d->dead) {
			assert(d->cb);
			ml->state_data = d;
			d->cb(d);

			if(ml->state == state_none) {
				return false;
			}
		}
	}

	return true;
}

static bool dispatch_timer(struct mainloop* ml) {
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	struct ml_timer* t = ml->timer.first;
	if(ml->state == state_dispatch_timer) {
		t = ml->state_data;
		assert(t);
		t = t->next;
	}

	ml->state = state_dispatch_timer;
	for(; t; t = t->next) {
		if(!t->enabled || t->dead) {
			continue;
		}

		struct timespec diff = t->time;
		timespec_subtract(&diff, &now);
		if(timespec_ms(&diff) <= 0) {
			ml->state_data = t;
			assert(t->cb);
			t->enabled = false;
			t->cb(t, &t->time);

			if(ml->state == state_none) {
				return false;
			}
		}
	}

	return true;
}

static bool dispatch_io(struct mainloop* ml, struct pollfd* fds, unsigned n_fds) {
	struct ml_io* io = ml->io.first;
	if(ml->state == state_dispatch_io) {
		io = ml->state_data;
		assert(io);
		io = io->next;
	}

	ml->state = state_dispatch_io;
	for(; io; io = io->next) {
		if(io->dead) {
			continue;
		}

		assert(io->fd_id < n_fds);
		struct pollfd* fd = &fds[io->fd_id];
		if(fd->revents) {
			ml->state_data = io;
			assert(io->cb);
			io->cb(io, map_flags_from_libc(fd->revents));

			if(ml->state == state_none) {
				return false;
			}
		}

		return true;
	}
}

static bool dispatch_custom(struct mainloop* ml, struct pollfd* fds,
		unsigned n_fds) {
	struct ml_custom* custom = ml->custom.first;
	if(ml->state == state_dispatch_custom) {
		custom = ml->state_data;
		assert(custom);
		custom = custom->next;
	}

	ml->state = state_dispatch_custom;
	for(struct ml_custom* c = ml->custom.first; c; c = c->next) {
		if(c->dead) {
			continue;
		}

		assert(custom->fds_id + custom->n_fds_last <= n_fds);
		struct pollfd* fd = &fds[custom->fds_id];
		ml->state_data = custom;

		// TODO: should dispatch always be called?
		// we could only call it if one of its fds has an revent or if
		// its timeout has timed out. But on the other hand, we can
		// also simply leave this check to the dispatch function...
		c->impl->dispatch(c, fd, c->n_fds_last);
		if(ml->state == state_none) {
			return false;
		}
	}

	return true;
}

void mainloop_dispatch(struct mainloop* ml, struct pollfd* fds, unsigned n_fds) {
	assert(ml->state != state_none);
	assert(ml->state != state_prepared);
	assert(ml->state != state_preparing);
	++ml->dispatch_depth;

	switch(ml->state) {
		case state_none: // fallthrough
		case state_dispatch_defer:
			if(!dispatch_defer(ml)) break; // fallthrough
		case state_dispatch_timer:
			if(!dispatch_timer(ml)) break; // fallthrough
		case state_dispatch_io:
			if(!dispatch_io(ml, fds, n_fds)) break; // fallthrough
		case state_dispatch_custom:
			dispatch_custom(ml, fds, n_fds);
			break;
		default:
			assert(false);
	}

	--ml->dispatch_depth;
	ml->state = state_none;
	ml->state_data = NULL;
}

int mainloop_iterate(struct mainloop* ml, bool block) {
	mainloop_prepare(ml);
	if(!block) {
		ml->prepared_timeout = 0;
	}

	int ret = mainloop_poll(ml);
	if(ret < 0) {
		return ret;
	}

	mainloop_dispatch(ml, ml->fds, ml->n_fds);
	return 0;
}

unsigned mainloop_query(struct mainloop* ml, struct pollfd* fds, unsigned n_fds,
		int* timeout) {
	assert(ml->state != state_preparing);
	assert(ml->state != state_none);
	assert(ml->state != state_polled);

	// dispatching not finished yet
	if(ml->state != state_prepared) {
		*timeout = 0;
		return 0;
	}

	unsigned size = min(n_fds, ml->n_fds) * sizeof(*fds);
	memcpy(fds, ml->fds, size);
	*timeout = ml->prepared_timeout;
	return ml->n_fds;
}

// ml_io
struct ml_io* ml_io_new(struct mainloop* ml, int fd,
		enum ml_io_flags events, ml_io_cb cb) {
	assert(ml);
	assert(fd >= 0);
	assert(cb);

	struct ml_io* io = calloc(1, sizeof(*io));
	io->mainloop = ml;
	io->fd = fd;
	io->events = events;
	io->cb = cb;
	io->fd_id = UINT_MAX;

	ml->rebuild_fds = true;
	++ml->n_io;

	if(!ml->io.first) {
		ml->io.first = io;
	} else {
		ml->io.last->next = io;
		io->prev = ml->io.last;
	}
	ml->io.last = io;

	return io;
}

void ml_io_set_data(struct ml_io* io, void* data) {
	assert(!io->dead);
	io->data = data;
}

void* ml_io_get_data(struct ml_io* io) {
	assert(!io->dead);
	return io->data;
}

int ml_io_get_fd(struct ml_io* io) {
	assert(!io->dead);
	return io->fd;
}

void ml_io_destroy(struct ml_io* io) {
	if(!io) {
		return;
	}

	struct mainloop* ml = io->mainloop;
	assert(ml);
	assert(!io->dead);

	--ml->n_io;
	if(ml->state_data == io) {
		io->dead = true;
		++io->mainloop->n_dead_io;
	} else {
		ml->rebuild_fds = true;
		destroy_io(io);
		free(io);
	}
}

void ml_io_events(struct ml_io* io, enum ml_io_flags events) {
	assert(!io->dead);
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

	if(!ml->timer.first) {
		ml->timer.first = timer;
	} else {
		ml->timer.last->next = timer;
		timer->prev = ml->timer.last;
	}
	ml->timer.last = timer;

	return timer;
}

void ml_timer_restart(struct ml_timer* timer, const struct timespec* time) {
	assert(!timer->dead);
	timer->enabled = time;
	if(time) {
		timer->time = *time;
	}
}

void ml_timer_set_data(struct ml_timer* timer, void* data) {
	assert(!timer->dead);
	timer->data = data;
}

void* ml_timer_get_data(struct ml_timer* timer) {
	assert(!timer->dead);
	return timer->data;
}

void ml_timer_destroy(struct ml_timer* timer) {
	if(!timer) {
		return;
	}

	assert(!timer->dead);
	timer->enabled = false;
	timer->dead = true;
	++timer->mainloop->n_dead_timer;
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

	if(!ml->defer.first) {
		ml->defer.first = defer;
	} else {
		ml->defer.last->next = defer;
		defer->prev = ml->defer.last;
	}
	ml->defer.last = defer;

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

	if(!ml->custom.first) {
		ml->custom.first = custom;
	} else {
		ml->custom.last->next = custom;
		custom->prev = ml->custom.last;
	}
	ml->custom.last = custom;

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
