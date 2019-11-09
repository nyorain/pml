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
// License along with pml; if not, see <http://www.gnu.org/licenses/>.
//
// NOTE: this doesn't share code with the original pulseaudio implementation,
// their copyright should probably be removed, right? I looked at their
// code for the initial sketch of this library but especially since the
// re-entrancy rework, this implementation is fundamentally different.
// The api is still roughly similar (but also has major differences by now)
// though. Not sure whether or not this is actually still a derived work,
// keeping their license for now.

#define _POSIX_C_SOURCE 200809L

#include "pml.h"
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>
#include <poll.h>

// Ideas:
// - the number of dispatched events from pml_iterate, allowing
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
// - support less timer delay by preparing the nearest timespec instead
//   of already calculating the resulting timeout interval.
//   And then calculate the timeout from that at the end of prepare.
//   For custom event sources: simply add timeout to the time 'query' was
//   called. Not sure if this is really needed though.
//   Maybe change the custom interface to use timespec instead?
//
// Optimizations:
// - implement the various caches and optimizations that keep track of
//   timeouts etc: keep track of next timer while timers are created/changed so
//   we don't have to iterate them in prepare
// - keep a list of enabled defer events?
// - rebuilding optimizations. On event source destruction
//   we could just set the fds to -1 (and potentially even re-use
//   them later on for different fds), instead of rebuilding.
//   Or: allow to change the fd on a pml_io?
// - implement (and use in pml_iterate):
// - Maybe also seperate pml.n_fds and capacity of pml.fds to
//   avoid reallocation in some cases.
//   ```
//   // Like `pml_dispatch` but additionally takes the return code from ret.
//   // This is only used for internal optimizations such as not even checking
//   // the returned fds if poll returned an error or 0.
//   void pml_dispatch_with_poll_code(struct pml*,
//   	struct pollfd* fds, unsigned n_fds, int poll_code);
//   ```

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

struct pml_io {
	struct pml_io* prev;
	struct pml_io* next;
	struct pml* pml;
	void* data;
	pml_io_cb cb;
	int fd;
	unsigned events;
	unsigned fd_id;
};

struct pml_timer {
	struct pml_timer* prev;
	struct pml_timer* next;
	struct pml* pml;
	struct timespec time;
	clockid_t clock;
	bool enabled;
	void* data;
	pml_timer_cb cb;
};

struct pml_defer {
	struct pml_defer* prev;
	struct pml_defer* next;
	struct pml* pml;
	void* data;
	pml_defer_cb cb;
	bool enabled;
};

struct pml_custom {
	struct pml_custom* prev;
	struct pml_custom* next;
	struct pml* pml;
	void* data;
	const struct pml_custom_impl* impl;
	unsigned fds_id;
	unsigned n_fds_last;
};

struct pml {
	unsigned n_io; // only alive ones
	unsigned n_fds;
	struct pollfd* fds;

	struct {
		struct pml_io* first;
		struct pml_io* last;
	} io;

	struct {
		struct pml_timer* first;
		struct pml_timer* last;
	} timer;

	struct {
		struct pml_defer* first;
		struct pml_defer* last;
	} defer;

	struct {
		struct pml_custom* first;
		struct pml_custom* last;
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

	// how many instances of pml_dispatch are currently on
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

static int64_t timespec_ms(const struct timespec* t) {
	return 1000 * t->tv_sec + t->tv_nsec / (1000 * 1000);
}

static void timespec_subtract(struct timespec* a, const struct timespec* minus) {
	a->tv_nsec -= minus->tv_nsec;
	a->tv_sec -= minus->tv_sec;
}

static void destroy_io(struct pml_io* io) {
	assert(io);
	if(io->next) io->next->prev = io->prev;
	if(io->prev) io->prev->next = io->next;
	if(io == io->pml->io.first) io->pml->io.first = io->next;
	if(io == io->pml->io.last) io->pml->io.last = io->prev;
	free(io);
}

static void destroy_timer(struct pml_timer* t) {
	assert(t);
	if(t->next) t->next->prev = t->prev;
	if(t->prev) t->prev->next = t->next;
	if(t == t->pml->timer.first) t->pml->timer.first = t->next;
	if(t == t->pml->timer.last) t->pml->timer.last = t->prev;
	free(t);
}

static void destroy_defer(struct pml_defer* d) {
	assert(d);
	if(d->next) d->next->prev = d->prev;
	if(d->prev) d->prev->next = d->next;
	if(d == d->pml->defer.first) d->pml->defer.first = d->next;
	if(d == d->pml->defer.last) d->pml->defer.last = d->prev;
	free(d);
}

static void destroy_custom(struct pml_custom* c) {
	assert(c);
	if(c->next) c->next->prev = c->prev;
	if(c->prev) c->prev->next = c->next;
	if(c == c->pml->custom.first) c->pml->custom.first = c->next;
	if(c == c->pml->custom.last) c->pml->custom.last = c->prev;
	free(c);
}

// mainloop
struct pml* pml_new(void) {
	struct pml* ml = calloc(1, sizeof(*ml));
	return ml;
}

void pml_destroy(struct pml* ml) {
	if(!ml) {
		return;
	}

	assert(ml->dispatch_depth == 0 &&
		"Destroying a mainloop that is still dispatching");
	if(ml->fds) {
		free(ml->fds);
	}

	// free all sources
	for(struct pml_custom* c = ml->custom.first; c;) {
		struct pml_custom* n = c->next;
		free(c);
		c = n;
	}
	for(struct pml_io* c = ml->io.first; c;) {
		struct pml_io* n = c->next;
		free(c);
		c = n;
	}
	for(struct pml_defer* c = ml->defer.first; c;) {
		struct pml_defer* n = c->next;
		free(c);
		c = n;
	}
	for(struct pml_timer* c = ml->timer.first; c;) {
		struct pml_timer* n = c->next;
		free(c);
		c = n;
	}

	free(ml);
}

void pml_prepare(struct pml* ml) {
	assert(ml);
	assert((ml->state == state_none || is_dispatch_state(ml->state)) &&
		"Invalid state of mainloop for pml_prepare");

	// dispatching isn't finished yet, just continue it
	// pml_poll will detect that as well, but we set the timeout
	// to zero so we can return it from pml_query
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
	for(struct pml_custom* c = ml->custom.first; c; c = c->next) {
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
	for(struct pml_timer* t = ml->timer.first; t; t = t->next) {
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
		for(struct pml_io* io = ml->io.first; io; io = io->next) {
			ml->fds[i].fd = io->fd;
			ml->fds[i].events = io->events;
			io->fd_id = i;
			++i;
		}

		for(struct pml_custom* c = ml->custom.first; c; c = c->next) {
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

int pml_poll(struct pml* ml, int timeout) {
	assert(ml);
	assert((ml->state == state_prepared || is_dispatch_state(ml->state)) &&
		"Invalid mainloop state for calling pml_poll");

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
		fprintf(stderr, "pml_poll: %s (%d)\n", strerror(errno), errno);
	}

	ml->state = state_polled;
	return ret;
}

static bool dispatch_defer(struct pml* ml) {
	// If we are continuing defer dispatching, the source to dispatch
	// is stored in state_data. Otherwise we start with the first one.
	struct pml_defer* d = ml->defer.first;
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

static bool dispatch_timer(struct pml* ml) {
	struct pml_timer* t = ml->timer.first;
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

static bool dispatch_io(struct pml* ml, struct pollfd* fds, unsigned n_fds) {
	struct pml_io* io = ml->io.first;
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

		assert(io->fd_id < n_fds && "Not enough fds passed to pml_dispatch");
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

static bool dispatch_custom(struct pml* ml, struct pollfd* fds,
		unsigned n_fds) {
	struct pml_custom* c = ml->custom.first;
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
			&& "Not enough fds passed to pml_dispatch");
		struct pollfd* fd = &fds[c->fds_id];
		c->impl->dispatch(c, fd, c->n_fds_last);
	}

	assert((ml->state == state_dispatch_custom || ml->state == state_none) &&
		"Inconsistent state change");
	return ml->state == state_dispatch_custom;
}

void pml_dispatch(struct pml* ml, struct pollfd* fds, unsigned n_fds) {
	assert(ml);
	assert((fds || !n_fds) &&
		"fds = NULL but n_fds != 0 passed to pml_dispatch");
	assert((ml->state == state_polled || is_dispatch_state(ml->state)) &&
		"Invalid mainloop state for calling pml_dispatch");
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

int pml_iterate(struct pml* ml, bool block) {
	assert(ml);

	pml_prepare(ml);
	if(!block) {
		ml->prepared_timeout = 0;
	}

	int ret = pml_poll(ml, ml->prepared_timeout);
	pml_dispatch(ml, ml->fds, ml->n_fds);
	return ret;
}

unsigned pml_query(struct pml* ml, struct pollfd* fds,
		unsigned n_fds, int* timeout) {
	assert(ml);
	assert(timeout && "timeout = NULL passed to pml_query");
	assert((!n_fds || fds) &&
		"fds = NULL but n_fds != 0 passed to pml_query");
	assert((ml->state == state_prepared || is_dispatch_state(ml->state)) &&
		"Invalid mainloop state for calling pml_query");

	// if dispatching isn't finished yet (i.e. is_dispatch_state_ml->state),
	// we still have to return the valid fds so we receive them
	// in pml_dispatch. ml->prepared_timeout was already set to 0
	// though
	unsigned size = min(n_fds, ml->n_fds) * sizeof(*fds);
	memcpy(fds, ml->fds, size);
	*timeout = ml->prepared_timeout;
	return ml->n_fds;
}

void pml_for_each_io(struct pml* ml, void (*cb)(struct pml_io*)) {
	assert(ml);
	assert(cb);

	struct pml_io* io;
	struct pml_io* next = ml->io.first;
	while((io = next)) {
		next = io->next;
		cb(io);
	}
}

void pml_for_each_timer(struct pml* ml, void (*cb)(struct pml_timer*)) {
	assert(ml);
	assert(cb);

	struct pml_timer* x;
	struct pml_timer* next = ml->timer.first;
	while((x = next)) {
		next = x->next;
		cb(x);
	}
}

void pml_for_each_defer(struct pml* ml, void (*cb)(struct pml_defer*)) {
	struct pml_defer* x;
	struct pml_defer* next = ml->defer.first;
	while((x = next)) {
		next = x->next;
		cb(x);
	}
}

void pml_for_each_custom(struct pml* ml, void (*cb)(struct pml_custom*)) {
	assert(ml);
	assert(cb);

	struct pml_custom* x;
	struct pml_custom* next = ml->custom.first;
	while((x = next)) {
		next = x->next;
		cb(x);
	}
}

// pml_io
struct pml_io* pml_io_new(struct pml* ml, int fd,
		unsigned events, pml_io_cb cb) {
	assert(ml);
	assert(fd >= 0);
	assert((events & (POLLERR | POLLHUP | POLLNVAL)) == 0);
	assert(cb);

	struct pml_io* io = calloc(1, sizeof(*io));
	io->pml = ml;
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

void pml_io_set_data(struct pml_io* io, void* data) {
	assert(io);
	io->data = data;
}

void* pml_io_get_data(struct pml_io* io) {
	assert(io);
	return io->data;
}

int pml_io_get_fd(struct pml_io* io) {
	assert(io);
	return io->fd;
}

void pml_io_destroy(struct pml_io* io) {
	if(!io) {
		return;
	}

	struct pml* ml = io->pml;
	assert(ml);

	--ml->n_io;
	if(ml->state_data == io) {
		assert(ml->state == state_dispatch_io);
		ml->state_data = io->next;
	}

	// in re-rentrant situations, the current fds array might be
	// returned from query without being rebuild after this.
	// pml_iterate itself won't poll but when the mainloop is
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

void pml_io_set_events(struct pml_io* io, unsigned events) {
	assert(io);
	io->events = events;
	if(io->fd_id != UINT_MAX && !io->pml->rebuild_fds) {
		io->pml->fds[io->fd_id].events = events;
	}
}

unsigned pml_io_get_events(struct pml_io* io) {
	return io->events;
}

struct pml* pml_io_get_pml(struct pml_io* io) {
	assert(io);
	assert(io->pml);
	return io->pml;
}

pml_io_cb pml_io_get_cb(struct pml_io* io) {
	assert(io);
	return io->cb;
}

// pml_timer
struct pml_timer* pml_timer_new(struct pml* ml,
		const struct timespec* time, pml_timer_cb cb) {
	assert(ml);
	assert(cb);

	struct pml_timer* timer = calloc(1, sizeof(*timer));
	timer->pml = ml;
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

void pml_timer_set_time(struct pml_timer* timer, struct timespec time) {
	assert(timer);
	timer->enabled = true;
	timer->time = time;
}

int pml_timer_set_time_rel(struct pml_timer* timer, struct timespec time) {
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

bool pml_timer_is_enabled(struct pml_timer* timer) {
	assert(timer);
	return timer->enabled;
}

void pml_timer_disable(struct pml_timer* timer) {
	assert(timer);
	timer->enabled = false;
}

void pml_timer_set_clock(struct pml_timer* timer, pml_clockid clock) {
	assert(timer);
	timer->clock = clock;
	timer->enabled = false;
}

struct timespec pml_timer_get_time(struct pml_timer* timer) {
	assert(timer);
	return timer->time;
}

clockid_t pml_timer_get_clock(struct pml_timer* timer) {
	assert(timer);
	return timer->clock;
}

void pml_timer_set_data(struct pml_timer* timer, void* data) {
	assert(timer);
	timer->data = data;
}

void* pml_timer_get_data(struct pml_timer* timer) {
	assert(timer);
	return timer->data;
}

void pml_timer_destroy(struct pml_timer* timer) {
	if(!timer) {
		return;
	}

	assert(timer->pml);
	if(timer->pml->state_data == timer) {
		assert(timer->pml->state == state_dispatch_timer);
		timer->pml->state_data = timer->next;
	}

	destroy_timer(timer);
}

struct pml* pml_timer_get_pml(struct pml_timer* timer) {
	assert(timer);
	assert(timer->pml);
	return timer->pml;
}

pml_timer_cb pml_timer_get_cb(struct pml_timer* timer) {
	assert(timer);
	return timer->cb;
}

// pml_defer
struct pml_defer* pml_defer_new(struct pml* ml, pml_defer_cb cb) {
	assert(ml);
	assert(cb);

	struct pml_defer* defer = calloc(1, sizeof(*defer));
	defer->cb = cb;
	defer->pml = ml;
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

void pml_defer_enable(struct pml_defer* defer, bool enable) {
	assert(defer);
	if(defer->enabled == enable) {
		return;
	}

	defer->enabled = enable;
	if(defer->enabled) {
		++defer->pml->n_enabled_defered;
	} else {
		--defer->pml->n_enabled_defered;
	}
}

void pml_defer_set_data(struct pml_defer* defer, void* data) {
	assert(defer);
	defer->data = data;
}

void* pml_defer_get_data(struct pml_defer* defer) {
	assert(defer);
	return defer->data;
}

void pml_defer_destroy(struct pml_defer* defer) {
	if(!defer) {
		return;
	}

	assert(defer->pml);
	if(defer->enabled) {
		--defer->pml->n_enabled_defered;
	}

	if(defer->pml->state_data == defer) {
		assert(defer->pml->state == state_dispatch_defer);
		defer->pml->state_data = defer->next;
	}

	destroy_defer(defer);
}

struct pml* pml_defer_get_pml(struct pml_defer* defer) {
	assert(defer);
	assert(defer->pml);
	return defer->pml;
}

pml_defer_cb pml_defer_get_cb(struct pml_defer* defer) {
	assert(defer);
	return defer->cb;
}

// pml_custom
struct pml_custom* pml_custom_new(struct pml* ml, const struct pml_custom_impl* impl) {
	assert(ml);
	assert(impl);
	assert(impl->dispatch);
	assert(impl->query);

	struct pml_custom* custom = calloc(1, sizeof(*custom));
	custom->pml = ml;
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

void pml_custom_set_data(struct pml_custom* custom, void* data) {
	assert(custom);
	custom->data = data;
}

void* pml_custom_get_data(struct pml_custom* custom) {
	assert(custom);
	return custom->data;
}

void pml_custom_destroy(struct pml_custom* custom) {
	if(!custom) {
		return;
	}

	struct pml* ml = custom->pml;
	assert(ml);

	if(ml->state_data == custom) {
		assert(ml->state == state_dispatch_custom);
		ml->state_data = custom->next;
	}

	// See pml_io_destroy for the reasoning. Basically: query (and external
	// polling) might happen before rebuilding
	assert(!custom->n_fds_last || custom->fds_id != UINT_MAX);
	for(unsigned i = 0u; i < custom->n_fds_last; ++i) {
		ml->fds[custom->fds_id + i].fd = -1;
	}

	// TODO(optimization) we might be able to avoid that in more situations;
	// do more efficient internal re-allocation in the fds array.
	if(custom->n_fds_last > 0) {
		ml->rebuild_fds = true;
	}

	destroy_custom(custom);
}

struct pml* pml_custom_get_pml(struct pml_custom* custom) {
	assert(custom);
	assert(custom->pml);
	return custom->pml;
}

const struct pml_custom_impl* pml_custom_get_impl(struct pml_custom* custom) {
	assert(custom);
	return custom->impl;
}
