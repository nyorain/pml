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
// Refactored version of the pulse audio default main loop implementation.
// Seemed nice to me since it has a rather clean and minimal interface,
// is quite flexible and has no external dependencies.
// See pulseaudio src/pulse/mainloop.c and <pulse/mainloop-api.h> for
// the original implementation.
// Added custom event sources that allow integration with pretty much
// anything (you could poll multiple event loops like this).

#pragma once

#include <stdbool.h>
#include <time.h>

struct timespec;
struct pollfd;

struct mainloop;

void mainloop_prepare(struct mainloop*);
void mainloop_poll(struct mainloop*);
void mainloop_dispatch(struct mainloop*);
void mainloop_iterate(struct mainloop*);
// struct pollfd* mainloop_get_fds(struct mainloop*, unsigned* count);

struct ml_io;
struct ml_timer;
struct ml_defer;
struct ml_custom;

enum ml_io_flags {
    ml_io_none = 0,
    ml_io_input = 1,
    ml_io_output = 2,
    ml_io_hangup = 4,
    ml_io_error = 8
};

// An IO event callback prototype
typedef void (*ml_io_cb)(struct ml_io* e, enum ml_io_flags revents);
// A IO event destroy callback prototype
typedef void (*ml_io_destroy_cb)(struct ml_io *e);

struct ml_io* ml_io_new(struct mainloop*, int fd, enum ml_io_flags events, ml_io_cb);
void ml_io_set_data(struct ml_io*, void*);
void* ml_io_get_data(struct ml_io*);
int ml_io_get_fd(struct ml_io*);
void ml_io_destroy(struct ml_io*);
void ml_io_set_destroy_db(struct ml_io*, ml_io_destroy_cb);
void ml_io_events(struct ml_io*, enum ml_io_flags);
struct mainloop* ml_io_get_mainloop(struct ml_io*);

// A time event callback prototype
typedef void (*ml_timer_cb)(struct ml_timer* e, const struct timespec *tv);
// A time event destroy callback prototype
typedef void (*ml_timer_destroy_cb)(struct ml_timer *e);

struct ml_timer* ml_timer_new(struct mainloop*, const struct timespec *tv, ml_timer_cb);
void ml_timer_restart(struct ml_timer*, const struct timespec* tv);
void ml_timer_set_data(struct ml_timer*, void*);
void* ml_timer_get_data(struct ml_timer*);
void ml_timer_destroy(struct ml_timer*);
void ml_timer_set_destroy_db(struct ml_timer*, ml_timer_destroy_cb);
struct mainloop* ml_timer_get_mainloop(struct ml_timer*);

// A defer event callback prototype
typedef void (*ml_defer_cb)(struct ml_defer* e);
// A defer event destroy callback prototype
typedef void (*ml_defer_destroy_cb)(struct ml_defer *e);

struct ml_defer* ml_defer_new(struct mainloop*, ml_defer_cb);
void ml_defer_enable(struct ml_defer*, bool enable);
void ml_defer_set_data(struct ml_defer*, void*);
void* ml_defer_get_data(struct ml_defer*);
void ml_defer_destroy(struct ml_defer*);
void ml_defer_set_destroy_db(struct ml_defer*, ml_timer_destroy_cb);
struct mainloop* ml_defer_get_mainloop(struct ml_defer*);

// TODO: add timeout functionality
struct ml_custom_impl {
	void (*prepare)(struct ml_custom*);
	void (*get_fds)(struct ml_custom*, unsigned* count, struct pollfd*);
	void (*dispatch)(struct ml_custom*, unsigned count, struct pollfd*);
};

struct ml_custom* ml_custom_new(struct mainloop*, const struct ml_custom_impl*);
void ml_custom_set_data(struct ml_custom*, void*);
void* ml_custom_get_data(struct ml_custom*);
void ml_custom_destroy(struct ml_custom*);
struct mainloop* ml_custom_get_mainloop(struct ml_custom*);
