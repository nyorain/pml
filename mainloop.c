#include "mainloop.h"
#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <assert.h>
#include <sys/poll.h>

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
	int prepared_timeout;
	int n_enabled_defered;
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

// mainloop
void mainloop_prepare(struct mainloop* ml) {
	// if there are enabled defer sources, we will simply dispatch
	// those and not care about the rest
	if(ml->n_enabled_defered) {
		return;
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

void mainloop_poll(struct mainloop* ml) {
	if(ml->n_enabled_defered > 0) {
		return;
	}

	int timeout = -1; // TODO
	int ret = poll(ml->fds, ml->n_io, timeout);
	// TODO
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
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	// TODO, wip
	for(struct ml_timer* t = ml->timer_list; t; t = t->next) {
		if(t->time <= now) {
			assert(t->cb);
			t->cb(t, now);
		}
	}

	// dispatch pollfds
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

void mainloop_iterate(struct mainloop* ml) {
	mainloop_prepare(ml);
	mainloop_poll(ml);
	mainloop_dispatch(ml);
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

	io->prev = mainloop->io_list;
	mainloop->io_list->next = io;

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
	if(io->destroy_cb) {
		io->destroy_cb(io);
	}

	if(io->next) {
		io->next->prev = io->prev;
	}
	if(io->prev) {
		io->prev->next = io->next;
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

struct ml_timer* ml_timer_new(struct mainloop*, const struct timespec *tv, ml_timer_cb);
void ml_timer_restart(struct ml_timer*, const struct timespec* tv);
void ml_timer_set_data(struct ml_timer*, void*);
void* ml_timer_get_data(struct ml_timer*);
void ml_timer_destroy(struct ml_timer*);
void ml_timer_set_destroy_db(struct ml_timer*, ml_timer_destroy_cb);
struct mainloop* ml_timer_get_mainloop(struct ml_timer*);

struct ml_defer* ml_defer_new(struct mainloop*, ml_defer_cb);
void ml_defer_enable(struct ml_defer*, bool enable); // set ml->n_enabled_defered
void ml_defer_set_data(struct ml_defer*, void*);
void* ml_defer_get_data(struct ml_defer*);
void ml_defer_destroy(struct ml_defer*);
void ml_defer_set_destroy_db(struct ml_defer*, ml_timer_destroy_cb);
struct mainloop* ml_defer_get_mainloop(struct ml_defer*);

struct ml_custom* ml_custom_new(struct mainloop*, const struct ml_custom_impl* impl);
void ml_custom_set_data(struct ml_custom*, void*);
void* ml_custom_get_data(struct ml_custom*);
void ml_custom_destroy(struct ml_custom*);
struct mainloop* ml_custom_get_mainloop(struct ml_custom*);
