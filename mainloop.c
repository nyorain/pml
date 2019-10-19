#include "mainloop.h"
#include <sys/poll.h>
#include <stdlib.h>
#include <limits.h>

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
	unsigned fds_start;
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

// mainloop
void mainloop_prepare(struct mainloop* ml) {
	if(ml->rebuild_fds) {
		ml->rebuild_fds = false;
		// TODO
	}
}

void mainloop_poll(struct mainloop* ml) {
	if(ml->n_enabled_defered > 0) {
		return;
	}

	int timeout = -1;
	int ret = poll(ml->fds, ml->n_io, timeout);
}

void mainloop_dispatch(struct mainloop* ml) {
	if(ml->n_enabled_defered) {
		// dispatch deferred
		return;
	}

	// dispatch time events
	// dispatch io events
	// dispatch custom events
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

struct ml_timer* ml_timer_new(struct mainloop*, const struct timeval *tv, ml_timer_cb);
void ml_timer_restart(struct ml_timer*, const struct timeval* tv);
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
