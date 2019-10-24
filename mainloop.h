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
//
// Refactored version of the pulse audio main loop implementation.
// Seemed nice to me since it has a rather clean and minimal interface,
// is quite flexible and has no external dependencies. Does not use
// any linux specifics (such as epoll), relies only on posix functionality.
// For more advanced usage scenarios, linux specifics (epoll) can improve
// performance though (e.g. many hundreds file descriptors which are not
// changed often), see e.g. sd-event, libuv or libev.
// See pulseaudio src/pulse/mainloop.c and <pulse/mainloop-api.h> for
// the original implementation.
// Added custom event sources that allow integration with pretty much
// anything (you could poll multiple event loops like this). Also
// changed some of the interfaces to be even more minimalistic to use
// and easier to integrate with stuff.

#pragma once

#include <stdbool.h>
#include <time.h>

struct timespec;
struct pollfd;

// Opaque structure representing all information about the mainloop.
struct mainloop;

// Creates a new, empty mainloop.
// Must be freed using mainloop_destroy.
struct mainloop* mainloop_new(void);

// Destroying the mainloop will automatically destroy all sources.
// They must not be used anymore after this.
// The mainloop itself must not be used after this.
void mainloop_destroy(struct mainloop*);

// Performs one iteration on the mainloop. Will block until a file
// descriptor becomes available or a timer times out.
// If there are enabled deferred events, will just dispatch those.
// See the 'prepare', 'query', 'poll', 'dispatch' functions below
// if you need more fine-grained control.
int mainloop_iterate(struct mainloop*);

// Prepares the mainloop for polling, i.e. builds internal data structures
// and the timeout to be used for polling.
// Therefore this call should be followed as soon as possible by
// mainloop_poll. The more time between this call and mainloop_poll/
// mainloop_dispatch, the more delay timers have.
void mainloop_prepare(struct mainloop*);

// If you want to intergrate the mainloop with an external mainloop or
// poll manually (or use another mechanism) you can call this function
// *after* calling mainloop_prepare to get the prepared file descriptor
// array and timeout. The buffer for the pollfd values is provided
// by the caller.
// - fds: an array with at least size n_fds into which the file descriptors
//   and events to be polled will be written.
//   Can be NULL if n_fds is 0 (useful to just query the number of
//   available fds before allocating).
// - n_fds: the length of fds
// - timeout: will be set to the prepared timeout.
//   -1 means that there is no active timer event and polling should happen
//   without timeout. 0 means that polling shouldn't happen at all.
//   In this case, one can skip the polling step directly and
//   call mainloop_dispatch.
// Will always return the number of internally avilable fds. If n_fds
// is smaller than this number, will only write the first n_fds values of fds.
// Otherwise, if n_fds is greater, will not modify the remaining values in fds.
unsigned mainloop_query(struct mainloop*, struct pollfd* fds,
	unsigned n_fds, int* timeout);

// Polls the mainloop, using the prepared information.
// Must be called after mainloop_prepared.
// Will return the number of available file descriptors, 0 if polling
// timed out or a negative number if an error ocurred (errno will be set
// from poll), basically the return value from poll.
// But will ignore signals, i.e. continue polling in that case.
int mainloop_poll(struct mainloop*);

// Dispatches all ready callbacks.
// Must be called after mainloop_poll (or mainloop_query in case the
// timeout was 0). In this case NULL and 0 can be provided for fds and n_fds.
// - fds: the pollfd values from mainloop_query, now filled with the
//   revents from poll.
// - n_fds: number of elements in the 'fds' array.
// The data in fds and n_fds (except revents) should match what was
// returned by mainloop_query.
// After this call, one iteration of the mainloop is complete and
// the next iteration can be started using 'mainloop_prepare'.
void mainloop_dispatch(struct mainloop*, struct pollfd* fds, unsigned n_fds);

struct ml_io;
struct ml_timer;
struct ml_defer;
struct ml_custom;

// Correspond to the POSIX POLL* values in poll.h
enum ml_io_flags {
    ml_io_none = 0,
    ml_io_input = 1,
    ml_io_output = 2,
    ml_io_hangup = 4,
    ml_io_error = 8
};

// ml_io represents an event source for a single fd.
typedef void (*ml_io_cb)(struct ml_io* e, enum ml_io_flags revents);
typedef void (*ml_io_destroy_cb)(struct ml_io *e);

struct ml_io* ml_io_new(struct mainloop*, int fd, enum ml_io_flags events, ml_io_cb);
void ml_io_set_data(struct ml_io*, void*);
void* ml_io_get_data(struct ml_io*);
int ml_io_get_fd(struct ml_io*);
void ml_io_destroy(struct ml_io*);
void ml_io_set_destroy_cb(struct ml_io*, ml_io_destroy_cb);
void ml_io_events(struct ml_io*, enum ml_io_flags);
struct mainloop* ml_io_get_mainloop(struct ml_io*);

// ml_timer
// The passed timerspec values represent the timepoints using CLOCK_REALTIME
// at which the timer should be triggered. They don't represent intervals.
typedef void (*ml_timer_cb)(struct ml_timer* e, const struct timespec*);
typedef void (*ml_timer_destroy_cb)(struct ml_timer *e);

struct ml_timer* ml_timer_new(struct mainloop*, const struct timespec*, ml_timer_cb);
void ml_timer_restart(struct ml_timer*, const struct timespec*);
void ml_timer_set_data(struct ml_timer*, void*);
void* ml_timer_get_data(struct ml_timer*);
void ml_timer_destroy(struct ml_timer*);
void ml_timer_set_destroy_cb(struct ml_timer*, ml_timer_destroy_cb);
struct mainloop* ml_timer_get_mainloop(struct ml_timer*);

// ml_defer
typedef void (*ml_defer_cb)(struct ml_defer* e);
typedef void (*ml_defer_destroy_cb)(struct ml_defer *e);

struct ml_defer* ml_defer_new(struct mainloop*, ml_defer_cb);
void ml_defer_enable(struct ml_defer*, bool enable);
void ml_defer_set_data(struct ml_defer*, void*);
void* ml_defer_get_data(struct ml_defer*);
void ml_defer_destroy(struct ml_defer*);
void ml_defer_set_destroy_cb(struct ml_defer*, ml_defer_destroy_cb);
struct mainloop* ml_defer_get_mainloop(struct ml_defer*);

// ml_custom
// Useful to integrate other mainloops, e.g. glib.
// Notice how this interface is basically a mirror of the mainloop
// fine-grained iteration control interface.
// One could embed a mainloop into another mainloop using this.
struct ml_custom_impl {
	// Will be called during the mainloop prepare phase.
	// Can be used to build timeout and pollfds.
	// Optional, can be NULL.
	void (*prepare)(struct ml_custom*);
	// Queries the prepared fds and timeout.
	// Mandatory, i.e. must be implemented and not be NULL.
	// Will only be called after prepare was called. Might be called
	// multiple times though, must not change it values without another
	// function being called.
	// - pollfd: An array with length n_fds to which the avilable
	//   fds and events should be written. If n_fds is too small to write
	//   all internal fds, discard the overflow. Might be NULL
	//   if n_fds is 0.
	// - timeout: The timeout should be written in milliseconds to this.
	//   Negative value is infinite polling, zero means that something is
	//   already ready (i.e. there shouldn't be any polling at all and
	//   dispatch should be called as soon as possible).
	// Returns the number of pollfds available.
	unsigned (*query)(struct ml_custom*, struct pollfd*, unsigned n_fds, int* timeout);
	// Should dispatch all internal event sources using the filled
	// pollfds (with length n_fds). Guaranteed to be the same that
	// were returned from query, now filled with revents.
	// Mandatory, i.e. must be implemented and not be NULL.
	void (*dispatch)(struct ml_custom*, struct pollfd*, unsigned n_fds);
	// Called when this event source is destroyed (via wl_custom_destroy
	// or mainloop_destroy). Optional, can be NULL.
	void (*destroy)(struct ml_custom*);
};

struct ml_custom* ml_custom_new(struct mainloop*, const struct ml_custom_impl*);
void ml_custom_set_data(struct ml_custom*, void*);
void* ml_custom_get_data(struct ml_custom*);
void ml_custom_destroy(struct ml_custom*);
struct mainloop* ml_custom_get_mainloop(struct ml_custom*);
