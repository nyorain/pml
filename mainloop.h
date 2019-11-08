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
struct ml_io;
struct ml_defer;
struct ml_timer;
struct ml_custom;

// Creates a new, empty mainloop.
// Must be freed using mainloop_destroy.
struct mainloop* mainloop_new(void);

// Destroying the mainloop will automatically destroy all sources.
// They must not be used anymore after this.
// The mainloop itself must not be used after this.
void mainloop_destroy(struct mainloop*);

// Performs one iteration on the mainloop. If `block` is true, will block
// until a file descriptor becomes available or a timer times out. Otherwise
// might just return immediately, without any dispatched events.
// If there are enabled deferred events, will just dispatch those.
// See the 'prepare', 'query', 'poll', 'dispatch' functions below
// if you need more fine-grained control.
// Returns a negative value on error and 0 on success.
int mainloop_iterate(struct mainloop*, bool block);

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
// Must only be called after mainloop_prepared was called.
// But will ignore signals, i.e. continue polling in that case.
// Returns the return value from poll. Even if polling failed,
// mainloop_dispatch must be called to consider the iteration finished.
int mainloop_poll(struct mainloop*, int timeout);

// Dispatches all ready callbacks.
// Must be called after mainloop_poll, before starting a new iteration.
// - fds: the pollfd values from mainloop_query, now filled with the
//   revents from poll.
// - n_fds: number of elements in the 'fds' array.
// The data in fds and n_fds (except revents) should match what was
// returned by mainloop_query.
// After this call, one iteration of the mainloop is complete and
// the next iteration can be started using 'mainloop_prepare'.
void mainloop_dispatch(struct mainloop*, struct pollfd* fds, unsigned n_fds);

// Calls the provided iteration function with every event source of
// the respective type created for the mainlopop.
// The callback may destroy the given event source but must not
// destroy other event sources of the respective type.
void mainloop_for_each_io(struct mainloop*, void (*)(struct ml_io*));
void mainloop_for_each_timer(struct mainloop*, void (*)(struct ml_timer*));
void mainloop_for_each_defer(struct mainloop*, void (*)(struct ml_defer*));
void mainloop_for_each_custom(struct mainloop*, void (*)(struct ml_custom*));

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

struct ml_io* ml_io_new(struct mainloop*, int fd, enum ml_io_flags events, ml_io_cb);
void ml_io_set_data(struct ml_io*, void*);
void* ml_io_get_data(struct ml_io*);
int ml_io_get_fd(struct ml_io*);
void ml_io_destroy(struct ml_io*);
void ml_io_events(struct ml_io*, enum ml_io_flags);
ml_io_cb ml_io_get_cb(struct ml_io*);
struct mainloop* ml_io_get_mainloop(struct ml_io*);

// ml_timer
// The passed timerspec values represent the timepoints using CLOCK_REALTIME
// at which the timer should be triggered. They don't represent intervals.
typedef void (*ml_timer_cb)(struct ml_timer* e, const struct timespec*);

struct ml_timer* ml_timer_new(struct mainloop*, const struct timespec*, ml_timer_cb);
void ml_timer_restart(struct ml_timer*, const struct timespec*);
void ml_timer_set_data(struct ml_timer*, void*);
void* ml_timer_get_data(struct ml_timer*);
void ml_timer_destroy(struct ml_timer*);
ml_timer_cb ml_timer_get_cb(struct ml_timer*);
struct mainloop* ml_timer_get_mainloop(struct ml_timer*);

// ml_defer
typedef void (*ml_defer_cb)(struct ml_defer* e);

struct ml_defer* ml_defer_new(struct mainloop*, ml_defer_cb);
void ml_defer_enable(struct ml_defer*, bool enable);
void ml_defer_set_data(struct ml_defer*, void*);
void* ml_defer_get_data(struct ml_defer*);
void ml_defer_destroy(struct ml_defer*);
ml_defer_cb ml_defer_get_cb(struct ml_defer*);
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
	unsigned (*query)(struct ml_custom*, struct pollfd*, unsigned n_fds,
		int* timeout);
	// Should dispatch all internal event sources using the filled
	// pollfds (with length n_fds). Guaranteed to be the same that
	// were returned from query, now filled with revents.
	// Mandatory, i.e. must be implemented and not be NULL.
	// Note that dispatch being called doesn't mean that the pollfds
	// actually have data or that the timeout expired, this has to
	// be checked first.
	void (*dispatch)(struct ml_custom*, struct pollfd*, unsigned n_fds);
};

struct ml_custom* ml_custom_new(struct mainloop*, const struct ml_custom_impl*);
void ml_custom_set_data(struct ml_custom*, void*);
void* ml_custom_get_data(struct ml_custom*);
void ml_custom_destroy(struct ml_custom*);
const struct ml_custom_impl* ml_custom_get_impl(struct ml_custom*);
struct mainloop* ml_custom_get_mainloop(struct ml_custom*);

// Additional documentation
// ========================
//
// Waking up a poll:
// -----------------
//
// Waking the mainloop up from mainloop_iterate or mainloop_poll
// from another thread is not possible. If an application needs this feature,
// it can easily implement it as well as the mainloop could
// do it internally by adding a ml_io with a one side of a pipe and
// then simply write to the other end when wishing to wake up the
// polling. On linux, this can be done even more efficiently using
// eventfds.
//
// Multithreading:
// ---------------
//
// Neither mainloop nor event sources have any internal synchronization
// mechanisms. They also won't start any helper threads.
// That means, applications can use externally synchronize access to
// the mainloop and its sources, when needed.
// Since the mainloop doesn't use any global state, it is also possible
// to just multiple mainloops, e.g. one per thread.
//
// Re-entrance:
// ------------
//
// The mainloop was designed to be re-entrant in certain scenarios.
// In general, it is safe to start a new mainloop iteration during
// the dispatch phase, i.e. from inside timer, io or defer callbacks
// or in the ml_custom_impl.dispatch implementation.
// In the other functions of ml_custom_impl, re-entrance is generally not
// allowed. Changing other *non-custom* event sources from inside 'prepare'
// is allowed though (even destroying them).
//
// Random weird re-entrant situations:
// mainloop_iterate
// | some callback (e.g. timer/deferred/io/custom dispatch) on source S
// || mainloop_iterate
// ||| another callback that destroys source S
// || access source S. It is destroyed now though. Undefined behavior.
// If an event source nests an iteration, it must be prepared that its
// own source might have been destroyed. The mainloop will give no
// guarantees of keeping sources alive while they are in a callback
// if the source associated with the callback is destroyed during it.
// It will otherwise be prepared for this case though.
// The same counts for this even simpler scenario, here it becomes more
// obvious that any logic keeping S alive would be unexpected.
// | mainloop_iterate
// || some callback on source S
// ||| destroy(S)
// ||| using S here is obviously undefined behavior.
// Otherwise it is perfectly valid to destroy an event source from
// within its own callback. It just must not be used in any way afterwards.
//
// After an enable/disable or timer restart call returns its semantics have
// effect. There won't be any delayed callbacks afterwards from a higher
// mainloop iteration level. The same is true for fd events, there won't
// be any delayed false positives for events that weren't requested.
//
// In conclusion: you have to be careful when nesting mainloop iterations.
// Avoid it if you can, but it some situations it might be useful.
// Please report all bugs/unexpected behavior in the mainloop, testing
// this or thinking of all the possible weird cases is hard.
//
// Custom event sources:
// ---------------------
//
// The mainloop gives certain guarantees for custom event sources:
// - query or dispatch will never be called without prepare being called first
// - after prepare being called, there will be exactly one call of
//   dispatch before prepare might be called again. This call will not
//   happen if the source is destroyed in between though.
// - calls to query will only happen between a call to prepare and dispatch
// The conditions holds true even when the mainloop is using in re-entrant
// scenarios. Dispatch counts as called as soon as the callback starts.
// That means, if the custom implementation starts a mainloop iteration
// from within its dispatch callack, prepared might be called again.
// In turn, custom implementations are required to always return the
// same values from 'query' if 'prepared' wasn't called. They furthermore
// must not access the mainloop (i.e. start an iteration or use
// the detailed iteration iteration api) in any way during 'prepare' or
// 'query'
