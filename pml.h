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

// Simple poll-based POSIX mainloop implementation (pml).
// For more advanced usage scenarios, os-specifics (such as linux's epoll or
// BSD's kqueue) will bring better performance (e.g. many hundreds file
// descriptors which are not changed often), see e.g. sd-event, libuv or libev,
// which are better for those use cases.
// This project was initially inspired by the pulse audio mainloop (that's why
// those people are still in the license) since i really liked its interface,
// see src/pulse/mainloop.c and <pulse/mainloop-api.h> for the original
// implementation.
// By now, this doesn't really share much (any?) code with their
// implementation anymore and offers slightly different features.
// Removed some of their features for simplicity and added custom event
// sources, support for re-entrancy, timer clocks and easy intergration
// with external polling.

#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timespec; // <time.h> with _POSIX_C_SOURCE
struct pollfd; // <poll.h>

// Opaque structure holding all mainloop state.
struct pml;
struct pml_io;
struct pml_timer;
struct pml_defer;
struct pml_custom;

// Creates a new, empty mainloop.
// Must be destroyed using pml_destroy.
struct pml* pml_new(void);

// Destroying the mainloop will automatically destroy all sources.
// They must not be used anymore after this.
// The mainloop itself must not be used after this.
void pml_destroy(struct pml*);

// Performs one iteration on the mainloop. If `block` is true, will block
// until a file descriptor becomes available or a timer times out. Otherwise
// might just return immediately, without any dispatched events.
// If there are enabled deferred events, will just dispatch those.
// See the 'prepare', 'query', 'poll', 'dispatch' functions below
// if you need more fine-grained control.
// Returns a negative value on error and 0 on success.
int pml_iterate(struct pml*, bool block);

// Prepares the mainloop for polling, i.e. builds internal data structures
// and the timeout to be used for polling.
// Therefore this call should be followed as soon as possible by
// pml_poll. The more time between this call and pml_poll/pml_dispatch,
// the more delay timers have.
void pml_prepare(struct pml*);

// If you want to intergrate the mainloop with an external mainloop or
// poll manually (or use another mechanism) you can call this function
// *after* calling pml_prepare to get the prepared file descriptor
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
//   call pml_dispatch.
// Will always return the number of internally avilable fds. If n_fds
// is smaller than this number, will only write the first n_fds values of fds.
// Otherwise, if n_fds is greater, will not modify the remaining values in fds.
unsigned pml_query(struct pml*, struct pollfd* fds,
	unsigned n_fds, int* timeout);

// Polls the mainloop, using the prepared information.
// Must only be called after pml_prepare was called.
// But will ignore signals, i.e. continue polling in that case.
// Returns the return value from poll. Even if polling failed,
// pml_dispatch must be called to consider the iteration finished.
int pml_poll(struct pml*, int timeout);

// Dispatches all ready callbacks.
// Must be called after pml_poll, before starting a new iteration.
// - fds: the pollfd values from pml_query, now filled with the
//   revents from poll.
// - n_fds: number of elements in the 'fds' array.
// The data in fds and n_fds (except revents) must match what was
// returned by pml_query. Calling this with fewer or different
// fds (the pointer doesn't have to be the same though) than returned
// by query is an error.
// After this call, one iteration of the mainloop is complete and
// the next iteration can be started using 'pml_prepare'.
void pml_dispatch(struct pml*, struct pollfd* fds, unsigned n_fds);

// Calls the provided iteration function with every event source of
// the respective type created for the mainlopop.
// The callback may destroy the given event source but must not
// destroy other event sources of the respective type.
void pml_for_each_io(struct pml*, void (*)(struct pml_io*));
void pml_for_each_timer(struct pml*, void (*)(struct pml_timer*));
void pml_for_each_defer(struct pml*, void (*)(struct pml_defer*));
void pml_for_each_custom(struct pml*, void (*)(struct pml_custom*));


// pml_io represents an event source for a single fd.
// events and revents are flags from the POLLXXX values defined in <poll.h>.
// As with polling, the callback might be called with POLLERR, POLLHUP or
// POLLNVAL even though those values are not valid as events.
typedef void (*pml_io_cb)(struct pml_io* e, unsigned revents);

struct pml_io* pml_io_new(struct pml*, int fd, unsigned events, pml_io_cb);
void pml_io_set_data(struct pml_io*, void*);
void* pml_io_get_data(struct pml_io*);
int pml_io_get_fd(struct pml_io*);
void pml_io_destroy(struct pml_io*);
void pml_io_set_events(struct pml_io*, unsigned events);
unsigned pml_io_get_events(struct pml_io*);
pml_io_cb pml_io_get_cb(struct pml_io*);
struct pml* pml_io_get_pml(struct pml_io*);


// pml_timer
typedef void (*pml_timer_cb)(struct pml_timer* e);
typedef int pml_clockid; // clockid_t requires to define _POSIX_C_SOURCE

// Pass a null timespec to initially disable the timer.
// The initial clock is CLOCK_REALTIME (i.e. time since epoch).
struct pml_timer* pml_timer_new(struct pml*,
	const struct timespec*, pml_timer_cb);
// Enables the timer.
void pml_timer_set_time(struct pml_timer*, struct timespec);
// Enables the timer. In this case, timespec is relative, using the
// timer's clock. Returns the return value from clock_gettime.
// If clock_gettime returns an error, the timer gets disabled.
int pml_timer_set_time_rel(struct pml_timer*, struct timespec);
// This will automatically disable the timer since the previously
// set timespec doesn't sense anymore.
void pml_timer_set_clock(struct pml_timer*, pml_clockid);
void pml_timer_set_data(struct pml_timer*, void*);
void* pml_timer_get_data(struct pml_timer*);
void pml_timer_destroy(struct pml_timer*);
pml_timer_cb pml_timer_get_cb(struct pml_timer*);
// Disables the timer no matter what time is currently set.
void pml_timer_disable(struct pml_timer*);
bool pml_timer_is_enabled(struct pml_timer*);
// Will return undefined value if timer is disabled.
struct timespec pml_timer_get_time(struct pml_timer*);
pml_clockid pml_timer_get_clock(struct pml_timer*);
struct pml* pml_timer_get_pml(struct pml_timer*);


// pml_defer represents a single callback that is called during the
// next iteration of the mainloop. It won't be automatically disabled
// so for one-shot events, destroy or disable the pml_defer in the
// callback.
typedef void (*pml_defer_cb)(struct pml_defer* e);

struct pml_defer* pml_defer_new(struct pml*, pml_defer_cb);
void pml_defer_enable(struct pml_defer*, bool enable);
void pml_defer_set_data(struct pml_defer*, void*);
void* pml_defer_get_data(struct pml_defer*);
void pml_defer_destroy(struct pml_defer*);
pml_defer_cb pml_defer_get_cb(struct pml_defer*);
struct pml* pml_defer_get_pml(struct pml_defer*);


// pml_custom
// Useful to integrate other mainloops, e.g. glib.
// Notice how this interface is basically a mirror of the mainloop
// fine-grained iteration control interface.
// One could embed a mainloop into another mainloop using this.
struct pml_custom_impl {
	// Will be called during the mainloop prepare phase.
	// Can be used to build timeout and pollfds.
	// Optional, can be NULL.
	void (*prepare)(struct pml_custom*);
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
	unsigned (*query)(struct pml_custom*, struct pollfd*, unsigned n_fds,
		int* timeout);
	// Should dispatch all internal event sources using the filled
	// pollfds (with length n_fds). Guaranteed to be the same that
	// were returned from query, now filled with revents.
	// Mandatory, i.e. must be implemented and not be NULL.
	// Note that dispatch being called doesn't mean that the pollfds
	// actually have data or that the timeout expired, this has to
	// be checked first.
	void (*dispatch)(struct pml_custom*, struct pollfd*, unsigned n_fds);
};

struct pml_custom* pml_custom_new(struct pml*, const struct pml_custom_impl*);
void pml_custom_set_data(struct pml_custom*, void*);
void* pml_custom_get_data(struct pml_custom*);
void pml_custom_destroy(struct pml_custom*);
const struct pml_custom_impl* pml_custom_get_impl(struct pml_custom*);
struct pml* pml_custom_get_pml(struct pml_custom*);

#ifdef __cplusplus
}
#endif

// Additional documentation
// ========================
//
// Waking up a poll:
// -----------------
//
// Waking the mainloop up from pml_iterate or pml_poll
// from another thread is not possible. If an application needs this feature,
// it can easily implement it as well as the mainloop could
// do it internally by adding a pml_io with one side of a pipe and
// then simply write to the other end when wishing to wake up the
// polling. On linux, this can be done even more efficiently using
// eventfds.
//
// Multithreading:
// ---------------
//
// Neither mainloop nor event sources have any internal synchronization
// mechanisms. They also won't start any helper threads.
// That means, applications can (and have to) use external synchronization
// to acess the mainloop and its sources, when needed.
// Since the mainloop doesn't use any global state, it is also possible
// to just multiple mainloops, e.g. one per thread.
//
// Re-entrance:
// ------------
//
// The mainloop was designed to be re-entrant in certain scenarios.
// In general, it is safe to start a new mainloop iteration during
// the dispatch phase, i.e. from inside timer, io or defer callbacks
// or in the pml_custom_impl.dispatch implementation.
// In the other functions of pml_custom_impl, re-entrance is generally not
// allowed. Changing other *non-custom* event sources from inside 'prepare'
// is allowed though (even destroying them).
//
// Random weird re-entrant situations:
// pml_iterate
// | some callback (e.g. timer/deferred/io/custom dispatch) on source S
// || pml_iterate
// ||| another callback that destroys source S
// || access source S. It is destroyed now though. Undefined behavior.
// If an event source nests an iteration in which callbacks that destroy
// the event source might be called, it must be prepared that it
// might have been destroyed afterwards. The mainloop will give no
// guarantees of keeping sources alive while they are in a callback.
// It will otherwise be prepared for this case though.
// The same counts for this even simpler scenario, here it becomes more
// obvious that any logic keeping event sources alive would be unexpected.
// | pml_iterate
// || some callback on source S
// ||| destroy(S)
// ||| using S here is obviously undefined behavior.
// Otherwise it is perfectly valid to destroy an event source from
// within its own callback. It just must not be used in any way afterwards.
//
// Enabling/disabling defer sources or changing a timer's time takes effect
// immediately. There won't be any delayed callbacks afterwards from a previuos
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
//   happen if source or mainloop is destroyed in between though.
// - calls to query will only happen between a call to prepare and dispatch
// The conditions holds true even when the mainloop is using in re-entrant
// scenarios. Dispatch counts as called as soon as the callback starts.
// That means, if the custom implementation starts a mainloop iteration
// from within its dispatch callack, prepared might be called again.
// In turn, custom implementations are required to always return the
// same values from 'query' if 'prepared' wasn't called in between.
// They furthermore must not access the mainloop (i.e. start an iteration or
// use the detailed iteration iteration api) in any way during 'prepare' or
// 'query'.
