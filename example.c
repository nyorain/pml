#define _POSIX_C_SOURCE 201710L
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <sys/poll.h>
#include <pulse/mainloop-api.h>
#include <pulse/context.h>
#include <pulse/subscribe.h>
#include <playerctl/playerctl.h>
#include "pml.h"

bool run = true;
unsigned count = 0;

static void timer_cb(struct pml_timer* timer) {
	printf("timer\n");
	// if(++count == 10) {
	// 	run = false;
	// }

	struct timespec next = { .tv_sec = 5 };
	pml_timer_set_time_rel(timer, next);
}

static void fd_cb(struct pml_io* io, unsigned revents) {
	assert(revents == POLLIN);

	char buf[512];
	int size = 511;
	int ret;
	while((ret = read(STDIN_FILENO, buf, size)) == size) {
		buf[size] = '\0';
		printf("Got: %s", buf);
	}

	if(ret < 0) {
		printf("Error: %s (%d)\n", strerror(errno), errno);
		return;
	}

	if(ret > 0) { // replace newline at the end
		buf[ret - 1] = '\0';
	} else {
		buf[ret] = '\0';
	}

	printf("Got: '%s'\n", buf);
	if(strcmp(buf, "quit") == 0) {
		printf("quitting.\n");
		run = false;
	}
}

// paml_io
struct paml_io_data {
	void* data;
	pa_io_event_cb_t cb;
	pa_io_event_destroy_cb_t destroy_cb;
	struct pa_mainloop_api* api;
};

static void paml_io_cb(struct pml_io* io, unsigned revents) {
	struct paml_io_data* iod = pml_io_get_data(io);
	assert(iod->cb);

	int fd = pml_io_get_fd(io);
	pa_io_event_flags_t pa_revents =
		(revents & POLLIN ? PA_IO_EVENT_INPUT : 0) |
		(revents & POLLOUT ? PA_IO_EVENT_OUTPUT : 0) |
		(revents & POLLERR ? PA_IO_EVENT_ERROR : 0) |
		(revents & POLLHUP ? PA_IO_EVENT_HANGUP : 0);
	iod->cb(iod->api, (pa_io_event*) io, fd, pa_revents, iod->data);
}

static pa_io_event* paml_io_new(pa_mainloop_api* api, int fd,
		pa_io_event_flags_t pa_events, pa_io_event_cb_t cb, void* data) {
	struct pml* pml = (struct pml*) api->userdata;
	unsigned events =
		(pa_events & PA_IO_EVENT_INPUT ? POLLIN : 0) |
		(pa_events & PA_IO_EVENT_OUTPUT ? POLLOUT : 0);
	struct pml_io* io = pml_io_new(pml, fd, events, &paml_io_cb);

	struct paml_io_data* iod = calloc(1, sizeof(*iod));
	iod->data = data;
	iod->cb = cb;
	iod->api = api;
	pml_io_set_data(io, iod);
	return (pa_io_event*) io;
}

static void paml_io_enable(pa_io_event* e, pa_io_event_flags_t pa_events) {
	unsigned events =
		(pa_events & PA_IO_EVENT_INPUT ? POLLIN : 0) |
		(pa_events & PA_IO_EVENT_OUTPUT ? POLLOUT : 0);
	pml_io_set_events((struct pml_io*) e, events);
}

static void paml_io_free(pa_io_event* e) {
	if(!e) {
		return;
	}

	struct paml_io_data* dd = pml_io_get_data((struct pml_io*) e);
	if(dd->destroy_cb) {
		dd->destroy_cb(dd->api, e, dd->data);
	}
	free(dd);
	pml_io_destroy((struct pml_io*) e);
}

static void paml_io_set_destroy(pa_io_event* e, pa_io_event_destroy_cb_t cb) {
	struct paml_io_data* iod = pml_io_get_data((struct pml_io*) e);
	iod->destroy_cb = cb;
}

// paml_time
struct paml_time_data {
	void* data;
	pa_time_event_cb_t cb;
	pa_time_event_destroy_cb_t destroy_cb;
	struct pa_mainloop_api* api;
};

static void paml_time_cb(struct pml_timer* t) {
	struct paml_time_data* td = pml_timer_get_data(t);
	assert(td->cb);

	struct timespec time = pml_timer_get_time(t);
	struct timeval tv = {time.tv_sec, time.tv_nsec / 1000};
	td->cb(td->api, (pa_time_event*) t, &tv, td->data);
}

static pa_time_event* paml_time_new(pa_mainloop_api* api,
		const struct timeval* tv, pa_time_event_cb_t cb, void* data) {
	struct pml* pml = (struct pml*) api->userdata;
	struct timespec ts = { tv->tv_sec, tv->tv_usec * 1000 };
	struct pml_timer* t = pml_timer_new(pml, &ts, &paml_time_cb);

	struct paml_time_data* td = calloc(1, sizeof(*td));
	td->data = data;
	td->cb = cb;
	td->api = api;
	pml_timer_set_data(t, td);
	return (pa_time_event*) t;
}

static void paml_time_restart(pa_time_event* e, const struct timeval* tv) {
	if(!tv) {
		pml_timer_disable((struct pml_timer*) e);
	} else {
		struct timespec ts = {tv->tv_sec, 1000 * tv->tv_usec};
		pml_timer_set_time((struct pml_timer*) e, ts);
	}
}

static void paml_time_free(pa_time_event* e) {
	if(!e) {
		return;
	}

	struct paml_time_data* dd = pml_timer_get_data((struct pml_timer*) e);
	if(dd->destroy_cb) {
		dd->destroy_cb(dd->api, e, dd->data);
	}
	free(dd);
	pml_timer_destroy((struct pml_timer*) e);
}

static void paml_time_set_destroy(pa_time_event* e, pa_time_event_destroy_cb_t cb) {
	struct paml_time_data* td = pml_timer_get_data((struct pml_timer*) e);
	td->destroy_cb = cb;
}

// paml_defer
struct paml_defer_data {
	void* data;
	pa_defer_event_cb_t cb;
	pa_defer_event_destroy_cb_t destroy_cb;
	struct pa_mainloop_api* api;
};

static void paml_defer_cb(struct pml_defer* d) {
	struct paml_defer_data* dd = pml_defer_get_data(d);
	assert(dd->cb);
	dd->cb(dd->api, (pa_defer_event*) d, dd->data);
}

static pa_defer_event* paml_defer_new(pa_mainloop_api* api,
		pa_defer_event_cb_t cb, void* data) {
	struct pml* pml = (struct pml*) api->userdata;
	struct pml_defer* d = pml_defer_new(pml, &paml_defer_cb);

	struct paml_defer_data* dd = calloc(1, sizeof(*dd));
	dd->data = data;
	dd->cb = cb;
	dd->api = api;
	pml_defer_set_data(d, dd);
	return (pa_defer_event*) d;
}

static void paml_defer_enable(pa_defer_event* e, int enable) {
	pml_defer_enable((struct pml_defer*) e, (bool) enable);
}

static void paml_defer_free(pa_defer_event* e) {
	if(!e) {
		return;
	}

	struct paml_defer_data* dd = pml_defer_get_data((struct pml_defer*) e);
	if(dd->destroy_cb) {
		dd->destroy_cb(dd->api, e, dd->data);
	}
	free(dd);
	pml_defer_destroy((struct pml_defer*) e);
}

static void paml_defer_set_destroy(pa_defer_event* e, pa_defer_event_destroy_cb_t cb) {
	struct paml_defer_data* dd = pml_defer_get_data((struct pml_defer*) e);
	dd->destroy_cb = cb;
}

// other
static void paml_quit(pa_mainloop_api* api, int retval) {
	printf("paml_quit: not implemented\n");
}

static const struct pa_mainloop_api pulse_mainloop_api = {
	.io_new = paml_io_new,
	.io_enable = paml_io_enable,
	.io_free = paml_io_free,
	.io_set_destroy = paml_io_set_destroy,

	.time_new = paml_time_new,
	.time_restart = paml_time_restart,
	.time_free = paml_time_free,
	.time_set_destroy = paml_time_set_destroy,

	.defer_new = paml_defer_new,
	.defer_enable = paml_defer_enable,
	.defer_free = paml_defer_free,
	.defer_set_destroy = paml_defer_set_destroy,

	.quit = paml_quit,
};

bool pulse_event = false;
static void pactx_subscribe_cb(pa_context* pactx,
		pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
	printf("pulse event: type = %d, idx = %d\n", t, idx);
	pulse_event = true;
}

static void pactx_state_cb(pa_context* pactx, void* data) {
	int s = pa_context_get_state(pactx);
	printf("pulse state: %d\n", s);

	if(s != PA_CONTEXT_READY) {
		return;
	}

	pa_operation *o = NULL;
	pa_context_set_subscribe_callback(pactx, pactx_subscribe_cb, NULL);
	o = pa_context_subscribe(pactx,
		PA_SUBSCRIPTION_MASK_SINK|
		PA_SUBSCRIPTION_MASK_SOURCE|
		PA_SUBSCRIPTION_MASK_SINK_INPUT|
		PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT|
		PA_SUBSCRIPTION_MASK_MODULE|
		PA_SUBSCRIPTION_MASK_CLIENT|
		PA_SUBSCRIPTION_MASK_SAMPLE_CACHE|
		PA_SUBSCRIPTION_MASK_SERVER|
		PA_SUBSCRIPTION_MASK_CARD, NULL, NULL);
	assert(o);
	pa_operation_unref(o);

	// nest event loops (re-entrant) just for fun
	struct pml* pml = data;
	printf("waiting for pulse event\n");
	while(run && !pulse_event) {
		pml_iterate(pml, true);
	}
	printf("done\n");
}

static gboolean player_status_cb(PlayerctlPlayer* player,
		PlayerctlPlaybackStatus status, gpointer data) {
	printf("player changed status to %d\n", (int) status);
	return true;
}

static void glib_prepare(struct pml_custom* c) {
	GMainContext* ctx = pml_custom_get_data(c);
	gint prio;
	g_main_context_prepare(ctx, &prio);
}

static unsigned glib_query(struct pml_custom* c, struct pollfd* fds,
		unsigned n_fds, int* timeout) {
	GPollFD gfds[n_fds + 1];
	const gint prio = INT_MAX;
	GMainContext* ctx = pml_custom_get_data(c);
	unsigned ret = g_main_context_query(ctx, prio, timeout, gfds, n_fds);
	for(unsigned i = 0u; i < n_fds; ++i) {
		fds[i].events = gfds[i].events;
		fds[i].fd = gfds[i].fd;
	}
	return ret;
}

static void glib_dispatch(struct pml_custom* c, struct pollfd* fds, unsigned n_fds) {
	GPollFD gfds[n_fds + 1];
	for(unsigned i = 0u; i < n_fds; ++i) {
		gfds[i].events = fds[i].events;
		gfds[i].revents = fds[i].revents;
		gfds[i].fd = fds[i].fd;
	}
	GMainContext* ctx = pml_custom_get_data(c);
	g_main_context_check(ctx, INT_MAX, gfds, n_fds);
	g_main_context_dispatch(ctx);
}

static const struct pml_custom_impl glib_custom_impl = {
	.prepare = glib_prepare,
	.query = glib_query,
	.dispatch = glib_dispatch
};

void io_destroy_paml_cb(struct pml_io* io) {
	if(pml_io_get_cb(io) == paml_io_cb) {
		paml_io_free((pa_io_event*) io);
	}
}

void timer_destroy_paml_cb(struct pml_timer* t) {
	if(pml_timer_get_cb(t) == paml_time_cb) {
		paml_time_free((pa_time_event*) t);
	}
}

void defer_destroy_paml_cb(struct pml_defer* d) {
	if(pml_defer_get_cb(d) == paml_defer_cb) {
		paml_defer_free((pa_defer_event*) d);
	}
}

int main() {
	struct pml* pml = pml_new();

	// pml_timer source: triggers once every second
	struct pml_timer* timer = pml_timer_new(pml, NULL, &timer_cb);
	struct timespec time = { .tv_sec = 5 };
	pml_timer_set_clock(timer, CLOCK_MONOTONIC);
	pml_timer_set_time_rel(timer, time);

	// pml_io source: listing for input to stdin
	struct pml_io* io = pml_io_new(pml, STDIN_FILENO, POLLIN, &fd_cb);
	(void) io;

	// pml_custom source: wrap glib's mainloop to use playerctl
	GError* error = NULL;
	PlayerctlPlayerManager* manager = playerctl_player_manager_new(&error);
	if(error != NULL) {
		printf("Can't create playerctl manager: %s\n", error->message);
		return 2;
	}

	// initial player iteration
	// make sure every player has a status change signal connected
	GList* available_players = NULL;
    g_object_get(manager, "player-names", &available_players, NULL);
    for(GList* l = available_players; l != NULL; l = l->next) {
		PlayerctlPlayerName* name = l->data;

		PlayerctlPlayer* player = playerctl_player_new_from_name(name, &error);
		if(error != NULL) {
			printf("Can't create player: %s\n", error->message);
			continue;
		}

		g_signal_connect(G_OBJECT(player), "playback-status",
			G_CALLBACK(player_status_cb), NULL);
		playerctl_player_manager_manage_player(manager, player);
		g_object_unref(player);
	}

	GMainContext* gctx = g_main_context_default();
	g_main_context_acquire(gctx);
	struct pml_custom* glib_custom = pml_custom_new(pml, &glib_custom_impl);
	pml_custom_set_data(glib_custom, gctx);

	// pulseaudio: we use the pml mainloop api to implement the pulseaudio
	// mainloop interface. The use i developed this for: using this
	// in a project that doesn't want to give pulseaudio its own thread
	// but listen for events.
	struct pa_mainloop_api pa_api = pulse_mainloop_api;
	pa_api.userdata = pml;
	pa_context* pactx = pa_context_new(&pa_api, NULL);
    pa_context_set_state_callback(pactx, pactx_state_cb, pml);

	if(pa_context_connect(pactx, NULL, 0, NULL) < 0) {
        printf("pa_context_connect() failed: %s", strerror(pa_context_errno(pactx)));
		return 1;
    }

	while(run) {
		pml_iterate(pml, true);
	}

	g_main_context_release(gctx);
	g_object_unref(manager);

	pa_context_disconnect(pactx);
	pa_context_unref(pactx);

	// We have to destroy the pulse audio (paml) event sources manually
	// to make sure their data is freed and destruction callbacks triggered
	pml_for_each_io(pml, io_destroy_paml_cb);
	pml_for_each_timer(pml, timer_destroy_paml_cb);
	pml_for_each_defer(pml, defer_destroy_paml_cb);
	pml_destroy(pml);
}
