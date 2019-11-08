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

// TODO: make callbacks mutable? shouldn't be a problem right?
// TODO: rebuilding optimizations. On event source destruction
// we could just set the fds to -1 (and potentially even re-use
// them later on for different fds), instead of rebuilding.
// Or: allow to change the fd on a ml_io?
// TODO: implement the various caches and optimizations that
// keep track of timeouts etc
// - keep track of next timer while timers are created/changed so
//   we don't have to iterate them in prepare
// - keep a list of enabled defer events?
// TODO: support less timer delay by preparing a minimal timespec (epoch) instead
// of already calculating the resulting timeout interval.
// And then calculate the timeout directly before polling
// TODO: return the number of dispatched events from mainloop_iterate, allowing
// to dispatch *all* pending events (call it until the number if 0).
// Not always possible to implement for custom sources though i guess...
// We could instead return if a new source was added/a timer reset
// since then or something, that should be equivalent as we already
// dispatch all.
// TODO(optimization): implement (and use in mainloop_iterate):
// ```
// // Like `mainloop_dispatch` but additionally takes the return code from ret.
// // This is only used for internal optimizations such as not even checking
// // the returned fds if poll returned an error or 0.
// void mainloop_dispatch_with_poll_code(struct mainloop*,
// 	struct pollfd* fds, unsigned n_fds, int poll_code);
// ```

// additional features to look into
// - supports other clocks than CLOCK_REALTIME

enum state {
	state_none = 0,
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
};

struct ml_timer {
	struct ml_timer* prev;
	struct ml_timer* next;
	struct mainloop* mainloop;
	struct timespec time;
	bool enabled;
	void* data;
	ml_timer_cb cb;
};

struct ml_defer {
	struct ml_defer* prev;
	struct ml_defer* next;
	struct mainloop* mainloop;
	void* data;
	ml_defer_cb cb;
	bool enabled;
};

struct ml_custom {
	struct ml_custom* prev;
	struct ml_custom* next;
	struct mainloop* mainloop;
	void* data;
	const struct ml_custom_impl* impl;
	unsigned fds_id;
	unsigned n_fds_last;
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
	free(t);
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
	free(c);
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

	assert(ml->dispatch_depth == 0);
	if(ml->fds) {
		free(ml->fds);
	}

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

	// dispatching isn't finished yet, just continue it
	if(ml->state != state_none) {
		printf("continuing dispatching\n");
		return;
	}

	ml->state = state_preparing;

	// start with custom sources since they may change stuff
	ml->prepared_timeout = -1;
	unsigned n_fds = ml->n_io;
	for(struct ml_custom* c = ml->custom.first; c; c = c->next) {
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
	if(ml->n_enabled_defered) {
		ml->prepared_timeout = 0;
	}

	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	for(struct ml_timer* t = ml->timer.first; t; t = t->next) {
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

	// rebuild fds if needed
	if(ml->rebuild_fds || n_fds > ml->n_fds) {
		ml->rebuild_fds = false;
		ml->fds = realloc(ml->fds, n_fds * sizeof(*ml->fds));
		ml->n_fds = n_fds;

		unsigned i = 0u;
		for(struct ml_io* io = ml->io.first; io; io = io->next) {
			ml->fds[i].fd = io->fd;
			ml->fds[i].events = map_flags_to_libc(io->events);
			io->fd_id = i;
			++i;
		}

		for(struct ml_custom* c = ml->custom.first; c; c = c->next) {
			int timeout;
			unsigned count = c->impl->query(c, &ml->fds[i], c->n_fds_last, &timeout);
			assert(count == c->n_fds_last &&
				"Custom event source changed number of fds without prepare");

			c->fds_id = i;
			i += count;
		}

		assert(i == ml->n_fds);
	}

	ml->state = state_prepared;
	return;
}

int mainloop_poll(struct mainloop* ml, int timeout) {
	assert(ml->state != state_none);
	assert(ml->state != state_preparing);
	assert(ml->state != state_polled);

	// dispatching not finished yet
	if(ml->state != state_prepared) {
		return 0;
	}

	do {
		// printf("timeout: %d\n", timeout);
		ml->poll_ret = poll(ml->fds, ml->n_fds, timeout);
	} while(ml->poll_ret < 0 && errno == EINTR);

	if(ml->poll_ret < 0) {
		fprintf(stderr, "mainloop poll: %s (%d)\n", strerror(errno), errno);
	}

	ml->state = state_polled;
	return ml->poll_ret;
}

static bool dispatch_defer(struct mainloop* ml) {
	struct ml_defer* d = ml->defer.first;
	if(ml->state == state_dispatch_defer) {
		d = ml->state_data;
	}

	ml->state = state_dispatch_defer;
	for(; d; d = ml->state_data) {
		ml->state_data = d->next;
		if(d->enabled) {
			assert(d->cb);
			d->cb(d);
		}
	}

	assert(ml->state == state_dispatch_defer || ml->state == state_none);
	return ml->state == state_dispatch_defer;
}

static bool dispatch_timer(struct mainloop* ml) {
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	struct ml_timer* t = ml->timer.first;
	if(ml->state == state_dispatch_timer) {
		t = ml->state_data;
	}

	ml->state = state_dispatch_timer;
	for(; t; t = ml->state_data) {
		ml->state_data = t->next;
		if(!t->enabled) {
			continue;
		}

		struct timespec diff = t->time;
		timespec_subtract(&diff, &now);
		if(timespec_ms(&diff) <= 0) {
			assert(t->cb);
			t->enabled = false;
			// special case of keep-alive logic. If t is destroyed
			// during this callback, we want the time parameter
			// to remain valid.
			const struct timespec copy = t->time;
			t->cb(t, &copy);
		}
	}

	assert(ml->state == state_dispatch_timer || ml->state == state_none);
	return ml->state == state_dispatch_timer;
}

static bool dispatch_io(struct mainloop* ml, struct pollfd* fds, unsigned n_fds) {
	struct ml_io* io = ml->io.first;
	if(ml->state == state_dispatch_io) {
		io = ml->state_data;
	}

	ml->state = state_dispatch_io;
	for(; io; io = ml->state_data) {
		ml->state_data = io->next;
		if(io->fd_id == UINT_MAX) {
			continue;
		}

		assert(io->fd_id < n_fds);
		struct pollfd* fd = &fds[io->fd_id];

		// Check here against fd->events again since the events might
		// have changed since we polled
		enum ml_io_flags revents = map_flags_from_libc(fd->revents);
		if(revents & io->events) {
			io->cb(io, revents & io->events);
		}
	}

	assert(ml->state == state_dispatch_io || ml->state == state_none);
	return ml->state == state_dispatch_io;
}

static bool dispatch_custom(struct mainloop* ml, struct pollfd* fds,
		unsigned n_fds) {
	struct ml_custom* c = ml->custom.first;
	if(ml->state == state_dispatch_custom) {
		c = ml->state_data;
	}

	ml->state = state_dispatch_custom;
	for(; c; c = ml->state_data) {
		ml->state_data = c->next;
		if(c->fds_id == UINT_MAX) {
			continue;
		}

		assert(c->fds_id + c->n_fds_last <= n_fds);
		struct pollfd* fd = &fds[c->fds_id];

		// TODO: should dispatch always be called?
		// we could only call it if one of its fds has an revent or if
		// its timeout has timed out. But on the other hand, we can
		// also simply leave this check to the dispatch function...
		c->impl->dispatch(c, fd, c->n_fds_last);
	}

	assert(ml->state == state_dispatch_custom || ml->state == state_none);
	return ml->state == state_dispatch_custom;
}

void mainloop_dispatch(struct mainloop* ml, struct pollfd* fds, unsigned n_fds) {
	assert(ml->state != state_none);
	assert(ml->state != state_prepared);
	assert(ml->state != state_preparing);
	++ml->dispatch_depth;
	// printf("dispatch_depth: %d\n", ml->dispatch_depth);

	switch(ml->state) {
		case state_polled: // fallthrough
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
			fprintf(stderr, "Invalid mainloop state %d\n", ml->state);
			assert(false);
			break;
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

	int ret = mainloop_poll(ml, ml->prepared_timeout);
	mainloop_dispatch(ml, ml->fds, ml->n_fds);
	return ret;
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

void mainloop_for_each_io(struct mainloop* ml, void (*cb)(struct ml_io*)) {
	struct ml_io* io;
	struct ml_io* next = ml->io.first;
	while((io = next)) {
		next = io->next;
		cb(io);
	}
}

void mainloop_for_each_timer(struct mainloop* ml, void (*cb)(struct ml_timer*)) {
	struct ml_timer* x;
	struct ml_timer* next = ml->timer.first;
	while((x = next)) {
		next = x->next;
		cb(x);
	}
}

void mainloop_for_each_defer(struct mainloop* ml, void (*cb)(struct ml_defer*)) {
	struct ml_defer* x;
	struct ml_defer* next = ml->defer.first;
	while((x = next)) {
		next = x->next;
		cb(x);
	}
}

void mainloop_for_each_custom(struct mainloop* ml, void (*cb)(struct ml_custom*)) {
	struct ml_custom* x;
	struct ml_custom* next = ml->custom.first;
	while((x = next)) {
		next = x->next;
		cb(x);
	}
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

	struct mainloop* ml = io->mainloop;
	assert(ml);

	--ml->n_io;
	if(ml->state_data == io) {
		assert(io->mainloop->state == state_dispatch_io);
		ml->state_data = io->next;
	}

	ml->rebuild_fds = true;
	destroy_io(io);
}

void ml_io_events(struct ml_io* io, enum ml_io_flags events) {
	io->events = events;
	if(io->fd_id != UINT_MAX && !io->mainloop->rebuild_fds) {
		io->mainloop->fds[io->fd_id].events = map_flags_to_libc(events);
	}
}

struct mainloop* ml_io_get_mainloop(struct ml_io* io) {
	return io->mainloop;
}

ml_io_cb ml_io_get_cb(struct ml_io* io) {
	return io->cb;
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

	assert(timer->mainloop);
	if(timer->mainloop->state_data == timer) {
		assert(timer->mainloop->state == state_dispatch_timer);
		timer->mainloop->state_data = timer->next;
	}

	destroy_timer(timer);
}

struct mainloop* ml_timer_get_mainloop(struct ml_timer* timer) {
	return timer->mainloop;
}

ml_timer_cb ml_timer_get_cb(struct ml_timer* timer) {
	return timer->cb;
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

	assert(defer->mainloop);
	if(defer->enabled) {
		--defer->mainloop->n_enabled_defered;
	}

	if(defer->mainloop->state_data == defer) {
		assert(defer->mainloop->state == state_dispatch_defer);
		defer->mainloop->state_data = defer->next;
	}

	destroy_defer(defer);
}

struct mainloop* ml_defer_get_mainloop(struct ml_defer* defer) {
	return defer->mainloop;
}

ml_defer_cb ml_defer_get_cb(struct ml_defer* defer) {
	return defer->cb;
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
	custom->fds_id = UINT_MAX;

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

	assert(custom->mainloop);
	if(custom->mainloop->state_data == custom) {
		assert(custom->mainloop->state == state_dispatch_custom);
		custom->mainloop->state_data = custom->next;
	}
	destroy_custom(custom);
}

struct mainloop* ml_custom_get_mainloop(struct ml_custom* custom) {
	return custom->mainloop;
}

const struct ml_custom_impl* ml_custom_get_impl(struct ml_custom* custom) {
	return custom->impl;
}
