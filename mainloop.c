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

#define _POSIX_C_SOURCE 200809L

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

// TODO: support less timer delay by preparing the nearest timespec instead
// of already calculating the resulting timeout interval.
// And then calculate the timeout from that at the end of prepare.
// For custom event sources: simply add timeout to the time 'query' was
// called. Not sure if this is really needed though.
// Maybe change the custom interface to use timespec instead?
// TODO: this doesn't share code with the original pulseaudio implementation,
// their copyright should probably be removed, right? The api is still roughly
// similar (but also has major differences by now) though
//
// Ideas:
// - the number of dispatched events from mainloop_iterate, allowing
//   to dispatch *all* pending events (call it until the number if 0).
//   Not always possible to implement for custom sources though i guess...
//   We could instead return if a new source was added/a timer reset
//   since then or something, that should be equivalent as we already
//   dispatch all.
// - add idle event sources? or set a flag in defer sources whether
//   they are urgent or not?
// - aren't defer callbacks essentially timer callbacks with a time
//   set to past timepoint (epoch 0)?
//   would probably simplify library to treat them the same. Only good idea
//   if it doesn't hurt performance though (atm timers aren't efficient).
// - fd, timer and defer callbacks could probably be made mutable if there
//   ever is a valid use case for it.
//
// Optimizations:
// - implement the various caches and optimizations that keep track of
//   timeouts etc: keep track of next timer while timers are created/changed so
//   we don't have to iterate them in prepare
// - keep a list of enabled defer events?
// - rebuilding optimizations. On event source destruction
//   we could just set the fds to -1 (and potentially even re-use
//   them later on for different fds), instead of rebuilding.
//   Or: allow to change the fd on a ml_io?
// - implement (and use in mainloop_iterate):
// - Maybe also seperate mainloop.n_fds and capacity of mainloop.fds to
//   avoid reallocation in some cases.
// ```
// // Like `mainloop_dispatch` but additionally takes the return code from ret.
// // This is only used for internal optimizations such as not even checking
// // the returned fds if poll returned an error or 0.
// void mainloop_dispatch_with_poll_code(struct mainloop*,
// 	struct pollfd* fds, unsigned n_fds, int poll_code);
// ```

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
	unsigned events;
	unsigned fd_id;
};

struct ml_timer {
	struct ml_timer* prev;
	struct ml_timer* next;
	struct mainloop* mainloop;
	struct timespec time;
	clockid_t clock;
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

	// We mainly need this to continue dispatching events where we
	// left off when dispatch is nested (re-entrancy).
	// When in a dispatching state, state_data holds the next event
	// source of the kind to dispatch. It must be NULL otherwise.
	enum state state;
	void* state_data;

	// how many instances of mainloop_dispatch are currently on
	// the stack. Usually this is 0 (not dispatching) or 1 (inside
	// a dispatch call). When using re-entrancy (only allowed for
	// callbacks triggered from dispatch) this can get higher.
	unsigned dispatch_depth;
};

static bool is_dispatch_state(enum state state) {
	return state == state_dispatch_io ||
		state == state_dispatch_defer ||
		state == state_dispatch_custom ||
		state == state_dispatch_timer;
}

static unsigned min(unsigned a, unsigned b) {
	return a < b ? a : b;
}

// static unsigned max(unsigned a, unsigned b) {
// 	return a > b ? a : b;
// }

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
	assert(io);
	if(io->next) io->next->prev = io->prev;
	if(io->prev) io->prev->next = io->next;
	if(io == io->mainloop->io.first) io->mainloop->io.first = io->next;
	if(io == io->mainloop->io.last) io->mainloop->io.last = io->prev;
	free(io);
}

static void destroy_timer(struct ml_timer* t) {
	assert(t);
	if(t->next) t->next->prev = t->prev;
	if(t->prev) t->prev->next = t->next;
	if(t == t->mainloop->timer.first) t->mainloop->timer.first = t->next;
	if(t == t->mainloop->timer.last) t->mainloop->timer.last = t->prev;
	free(t);
}

static void destroy_defer(struct ml_defer* d) {
	assert(d);
	if(d->next) d->next->prev = d->prev;
	if(d->prev) d->prev->next = d->next;
	if(d == d->mainloop->defer.first) d->mainloop->defer.first = d->next;
	if(d == d->mainloop->defer.last) d->mainloop->defer.last = d->prev;
	free(d);
}

static void destroy_custom(struct ml_custom* c) {
	assert(c);
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

	assert(ml->dispatch_depth == 0 &&
		"Destroying a mainloop that is still dispatching");
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
	assert(ml);
	assert((ml->state == state_none || is_dispatch_state(ml->state)) &&
		"Invalid state of mainloop for mainloop_prepare");

	// dispatching isn't finished yet, just continue it
	// mainloop_poll will detect that as well, but we set the timeout
	// to zero so we can return it from mainloop_query
	if(ml->state != state_none) {
		ml->prepared_timeout = 0;
		return;
	}

	ml->state = state_preparing;

	ml->prepared_timeout = -1;
	if(ml->n_enabled_defered) {
		ml->prepared_timeout = 0;
	}

	// prepare custom sources
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

	// timers
	struct timespec now;
	clockid_t clk = CLOCK_REALTIME;
	clock_gettime(clk, &now);
	for(struct ml_timer* t = ml->timer.first; t; t = t->next) {
		if(!t->enabled) {
			continue;
		}

		if(t->clock != clk) {
			clockid_t clk = t->clock;
			clock_gettime(clk, &now);
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
			ml->fds[i].events = io->events;
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

	assert(ml->state == state_preparing);
	ml->state = state_prepared;
	return;
}

int mainloop_poll(struct mainloop* ml, int timeout) {
	assert(ml);
	assert((ml->state == state_prepared || is_dispatch_state(ml->state)) &&
		"Invalid mainloop state for calling mainloop_poll");

	// dispatching not finished yet, we can skip polling.
	if(ml->state != state_prepared) {
		return 0;
	}

	int ret;
	// we ignore incoming signals
	do {
		ret = poll(ml->fds, ml->n_fds, timeout);
	} while(ret < 0 && errno == EINTR);

	if(ret < 0) {
		fprintf(stderr, "mainloop poll: %s (%d)\n", strerror(errno), errno);
	}

	ml->state = state_polled;
	return ret;
}

static bool dispatch_defer(struct mainloop* ml) {
	// If we are continuing defer dispatching, the source to dispatch
	// is stored in state_data. Otherwise we start with the first one.
	struct ml_defer* d = ml->defer.first;
	if(ml->state == state_dispatch_defer) {
		d = ml->state_data;
	}

	// This idiom is used for the other dispatch functions as well.
	// We store the state here and always set state_data to the next
	// source that is to be dispatched. When a callback then starts
	// a new mainloop iteration (i.e. uses re-entrancy), we detect
	// this case and continue dispatching with state_data (see above).
	// When the nested iteration is finished, state_data will be set to NULL
	// and state to state_none.
	// When control returns here then later on, the for loop will break (since
	// state_data is NULL) immediately after the callback and dispatching
	// furthermore end immediately since state is state_none.
	// Important for this to work: the callback must always be the last
	// thing we do in the loop body. By the time the callback returns
	// the event source (d) might actually already be destroyed.
	// When destroying an event source we also check whether it is currently
	// set as state_data. If so we just set the next source of this type
	// as state_data.
	ml->state = state_dispatch_defer;
	for(; d; d = ml->state_data) {
		ml->state_data = d->next;
		if(d->enabled) {
			assert(d->cb);
			d->cb(d);
		}
	}

	// If the state is state_none now, a callback triggered a
	// re-entrant iteration that continued/finished dispatching.
	assert((ml->state == state_dispatch_defer || ml->state == state_none) &&
		"Inconsistent state change");
	return ml->state == state_dispatch_defer;
}

static bool dispatch_timer(struct mainloop* ml) {
	struct ml_timer* t = ml->timer.first;
	if(ml->state == state_dispatch_timer) {
		t = ml->state_data;
	}

	struct timespec now;
	clockid_t clk = CLOCK_REALTIME;
	clock_gettime(clk, &now);

	ml->state = state_dispatch_timer;
	for(; t; t = ml->state_data) {
		ml->state_data = t->next;
		if(!t->enabled) {
			continue;
		}

		if(t->clock != clk) {
			clk = t->clock;
			clock_gettime(clk, &now);
		}

		struct timespec diff = t->time;
		timespec_subtract(&diff, &now);
		if(timespec_ms(&diff) <= 0) {
			assert(t->cb);
			t->enabled = false;
			t->cb(t);
		}
	}

	assert((ml->state == state_dispatch_timer || ml->state == state_none) &&
		"Inconsistent state change");
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

		// this means that this source was added since the last event source
		// preparation, i.e. this io's fd wasn't polled yet
		if(io->fd_id == UINT_MAX) {
			continue;
		}

		assert(io->fd_id < n_fds &&
			"Not enough fds passed to mainloop_dispatch");
		struct pollfd* fd = &fds[io->fd_id];
		assert(fd->fd >= 0);

		// Check here against fd->events again since the events might
		// have changed since we polled
		unsigned events = (io->events | POLLERR | POLLHUP | POLLNVAL);
		unsigned revents = fd->revents & events;
		if(revents) {
			io->cb(io, revents);
		}
	}

	assert((ml->state == state_dispatch_io || ml->state == state_none) &&
		"Inconsistent state change");
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

		// this means that this source was added since the last event
		// source preparation, i.e. this custom source
		// was never prepared and its fds were not polled
		if(c->fds_id == UINT_MAX) {
			continue;
		}

		assert(c->fds_id + c->n_fds_last <= n_fds
			&& "Not enough fds passed to mainloop_dispatch");
		struct pollfd* fd = &fds[c->fds_id];
		c->impl->dispatch(c, fd, c->n_fds_last);
	}

	assert((ml->state == state_dispatch_custom || ml->state == state_none) &&
		"Inconsistent state change");
	return ml->state == state_dispatch_custom;
}

void mainloop_dispatch(struct mainloop* ml, struct pollfd* fds, unsigned n_fds) {
	assert(ml);
	assert((fds || !n_fds) &&
		"fds = NULL but n_fds != 0 passed to mainloop_dispatch");
	assert((ml->state == state_polled || is_dispatch_state(ml->state)) &&
		"Invalid mainloop state for calling mainloop_dispatch");
	unsigned depth = ml->dispatch_depth;
	++ml->dispatch_depth;

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
			assert(false && "Invalid mainloop state");
			break;
	}

	--ml->dispatch_depth;
	assert(depth == ml->dispatch_depth && "Mainloop depth corrupted");
	ml->state = state_none;
	ml->state_data = NULL;
}

int mainloop_iterate(struct mainloop* ml, bool block) {
	assert(ml);

	mainloop_prepare(ml);
	if(!block) {
		ml->prepared_timeout = 0;
	}

	int ret = mainloop_poll(ml, ml->prepared_timeout);
	mainloop_dispatch(ml, ml->fds, ml->n_fds);
	return ret;
}

unsigned mainloop_query(struct mainloop* ml, struct pollfd* fds,
		unsigned n_fds, int* timeout) {
	assert(ml);
	assert(timeout && "timeout = NULL passed to mainloop_query");
	assert((!n_fds || fds) &&
		"fds = NULL but n_fds != 0 passed to mainloop_query");
	assert((ml->state == state_prepared || is_dispatch_state(ml->state)) &&
		"Invalid mainloop state for calling mainloop_query");

	// if dispatching isn't finished yet (i.e. is_dispatch_state_ml->state),
	// we still have to return the valid fds so we receive them
	// in mainloop_dispatch. ml->prepared_timeout was already set to 0
	// though
	unsigned size = min(n_fds, ml->n_fds) * sizeof(*fds);
	memcpy(fds, ml->fds, size);
	*timeout = ml->prepared_timeout;
	return ml->n_fds;
}

void mainloop_for_each_io(struct mainloop* ml, void (*cb)(struct ml_io*)) {
	assert(ml);
	assert(cb);

	struct ml_io* io;
	struct ml_io* next = ml->io.first;
	while((io = next)) {
		next = io->next;
		cb(io);
	}
}

void mainloop_for_each_timer(struct mainloop* ml, void (*cb)(struct ml_timer*)) {
	assert(ml);
	assert(cb);

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
	assert(ml);
	assert(cb);

	struct ml_custom* x;
	struct ml_custom* next = ml->custom.first;
	while((x = next)) {
		next = x->next;
		cb(x);
	}
}

// ml_io
struct ml_io* ml_io_new(struct mainloop* ml, int fd,
		unsigned events, ml_io_cb cb) {
	assert(ml);
	assert(fd >= 0);
	assert((events & (POLLERR | POLLHUP | POLLNVAL)) == 0);
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
	assert(io);
	io->data = data;
}

void* ml_io_get_data(struct ml_io* io) {
	assert(io);
	return io->data;
}

int ml_io_get_fd(struct ml_io* io) {
	assert(io);
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
		assert(ml->state == state_dispatch_io);
		ml->state_data = io->next;
	}

	// in re-rentrant situations, the current fds array might be
	// returned from query without being rebuild after this.
	// mainloop_iterate itself won't poll but when the mainloop is
	// integrated externally that might happen.
	// Since the fd might be destroyed after this and no longer be
	// valid, we just unset it here (poll ignores .fd = -1 entries)
	if(io->fd_id != UINT_MAX) {
		ml->fds[io->fd_id].fd = -1;
	}

	// TODO(optimiziation): we don't really have to set this here.
	// could potentially even re-use it later on when creating a new io.
	// sketch: build a linked list of free fd entries in fds by
	// setting their fds to -(id of next free entry) and storing
	// the first free entry (or -1) in mainloop.
	// Could even store "free blocks sizes" and do the same for custom
	// sources by using fds[i].events as block size.
	ml->rebuild_fds = true;
	destroy_io(io);
}

void ml_io_set_events(struct ml_io* io, unsigned events) {
	assert(io);
	io->events = events;
	if(io->fd_id != UINT_MAX && !io->mainloop->rebuild_fds) {
		io->mainloop->fds[io->fd_id].events = events;
	}
}

unsigned ml_io_get_events(struct ml_io* io) {
	return io->events;
}

struct mainloop* ml_io_get_mainloop(struct ml_io* io) {
	assert(io);
	assert(io->mainloop);
	return io->mainloop;
}

ml_io_cb ml_io_get_cb(struct ml_io* io) {
	assert(io);
	return io->cb;
}

// ml_timer
struct ml_timer* ml_timer_new(struct mainloop* ml,
		const struct timespec* time, ml_timer_cb cb) {
	assert(ml);
	assert(cb);

	struct ml_timer* timer = calloc(1, sizeof(*timer));
	timer->mainloop = ml;
	timer->cb = cb;
	timer->clock = CLOCK_REALTIME;
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

void ml_timer_set_time(struct ml_timer* timer, struct timespec time) {
	assert(timer);
	timer->enabled = true;
	timer->time = time;
}

int ml_timer_set_time_rel(struct ml_timer* timer, struct timespec time) {
	assert(timer);
	int res = clock_gettime(timer->clock, &timer->time);
	if(res != 0) {
		timer->enabled = false;
		printf("clock_gettime: %s (%d)\n", strerror(errno), errno);
		return res;
	}

	timer->enabled = true;
	timer->time.tv_nsec += time.tv_nsec;
	timer->time.tv_sec += time.tv_sec;
	return 0;
}

bool ml_timer_is_enabled(struct ml_timer* timer) {
	assert(timer);
	return timer->enabled;
}

void ml_timer_disable(struct ml_timer* timer) {
	assert(timer);
	timer->enabled = false;
}

void ml_timer_set_clock(struct ml_timer* timer, ml_clockid clock) {
	assert(timer);
	timer->clock = clock;
	timer->enabled = false;
}

struct timespec ml_timer_get_time(struct ml_timer* timer) {
	assert(timer);
	return timer->time;
}

clockid_t ml_timer_get_clock(struct ml_timer* timer) {
	assert(timer);
	return timer->clock;
}

void ml_timer_set_data(struct ml_timer* timer, void* data) {
	assert(timer);
	timer->data = data;
}

void* ml_timer_get_data(struct ml_timer* timer) {
	assert(timer);
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
	assert(timer);
	assert(timer->mainloop);
	return timer->mainloop;
}

ml_timer_cb ml_timer_get_cb(struct ml_timer* timer) {
	assert(timer);
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
	assert(defer);
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
	assert(defer);
	defer->data = data;
}

void* ml_defer_get_data(struct ml_defer* defer) {
	assert(defer);
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
	assert(defer);
	assert(defer->mainloop);
	return defer->mainloop;
}

ml_defer_cb ml_defer_get_cb(struct ml_defer* defer) {
	assert(defer);
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
	assert(custom);
	custom->data = data;
}

void* ml_custom_get_data(struct ml_custom* custom) {
	assert(custom);
	return custom->data;
}

void ml_custom_destroy(struct ml_custom* custom) {
	if(!custom) {
		return;
	}

	struct mainloop* ml = custom->mainloop;
	assert(ml);

	if(ml->state_data == custom) {
		assert(ml->state == state_dispatch_custom);
		ml->state_data = custom->next;
	}

	// See ml_io_destroy for the reasoning. Basically: query (and external
	// polling) might happen before rebuilding
	assert(!custom->n_fds_last || custom->fds_id != UINT_MAX);
	for(unsigned i = 0u; i < custom->n_fds_last; ++i) {
		ml->fds[custom->fds_id + i].fd = -1;
	}

	// TODO: we might be able to avoid that in more situations; do more
	// efficient internal re-allocation in the fds array.
	if(custom->n_fds_last > 0) {
		ml->rebuild_fds = true;
	}

	destroy_custom(custom);
}

struct mainloop* ml_custom_get_mainloop(struct ml_custom* custom) {
	assert(custom);
	assert(custom->mainloop);
	return custom->mainloop;
}

const struct ml_custom_impl* ml_custom_get_impl(struct ml_custom* custom) {
	assert(custom);
	return custom->impl;
}
