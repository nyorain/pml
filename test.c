#define _POSIX_C_SOURCE 201710L
#include <stdio.h>
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
#include "mainloop.h"

bool run = true;
unsigned count = 0;

static void timer_cb(struct ml_timer* timer, const struct timespec* t) {
	printf("timer\n");
	// if(++count == 10) {
	// 	run = false;
	// }

	struct timespec next;
	clock_gettime(CLOCK_REALTIME, &next);
	next.tv_sec += 1;
	ml_timer_restart(timer, &next);
}

static void fd_cb(struct ml_io* io, enum ml_io_flags revents) {
	assert(revents == ml_io_input);
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

	buf[ret] = '\0';
	printf("Got: %s", buf);
}

// paml_io
struct paml_io_data {
	void* data;
	pa_io_event_cb_t cb;
	pa_io_event_destroy_cb_t destroy_cb;
	struct pa_mainloop_api* api;
};

static void paml_io_cb(struct ml_io* io, enum ml_io_flags revents) {
	struct paml_io_data* iod = ml_io_get_data(io);
	assert(iod->cb);

	int fd = ml_io_get_fd(io);
	int pa_revents = (pa_io_event_flags_t) revents;
	iod->cb(iod->api, (pa_io_event*) io, fd, pa_revents, iod->data);
}

static void paml_io_destroy(struct ml_io* io) {
	struct paml_io_data* iod = ml_io_get_data(io);
	if(iod->destroy_cb) {
		iod->destroy_cb(iod->api, (pa_io_event*) io, iod->data);
	}
	free(iod);
}

static pa_io_event* paml_io_new(pa_mainloop_api* api, int fd,
		pa_io_event_flags_t pa_events, pa_io_event_cb_t cb, void* data) {
	struct mainloop* ml = (struct mainloop*) api->userdata;
	enum ml_io_flags events = (enum ml_io_flags) pa_events;
	struct ml_io* io = ml_io_new(ml, fd, events, &paml_io_cb);

	struct paml_io_data* iod = calloc(1, sizeof(*iod));
	iod->data = data;
	iod->cb = cb;
	iod->api = api;
	ml_io_set_data(io, iod);
	ml_io_set_destroy_db(io, &paml_io_destroy);
	return (pa_io_event*) io;
}

static void paml_io_enable(pa_io_event* e, pa_io_event_flags_t pa_events) {
	enum ml_io_flags events = (enum ml_io_flags) pa_events;
	ml_io_events((struct ml_io*) e, events);
}

static void paml_io_free(pa_io_event* e) {
	ml_io_destroy((struct ml_io*) e);
}

static void paml_io_set_destroy(pa_io_event* e, pa_io_event_destroy_cb_t cb) {
	struct paml_io_data* iod = ml_io_get_data((struct ml_io*) e);
	iod->destroy_cb = cb;
}

// paml_time
struct paml_time_data {
	void* data;
	pa_time_event_cb_t cb;
	pa_time_event_destroy_cb_t destroy_cb;
	struct pa_mainloop_api* api;
};

static void paml_time_cb(struct ml_timer* t, const struct timespec* time) {
	struct paml_time_data* td = ml_timer_get_data(t);
	assert(td->cb);

	struct timeval tv = {time->tv_sec, time->tv_nsec / 1000};
	td->cb(td->api, (pa_time_event*) td, &tv, td->data);
}

static void paml_time_destroy(struct ml_timer* t) {
	struct paml_time_data* td = ml_timer_get_data(t);
	if(td->destroy_cb) {
		td->destroy_cb(td->api, (pa_time_event*) td, td->data);
	}
	free(td);
}

static pa_time_event* paml_time_new(pa_mainloop_api* api,
		const struct timeval* tv, pa_time_event_cb_t cb, void* data) {
	struct mainloop* ml = (struct mainloop*) api->userdata;
	struct timespec ts = { tv->tv_sec, tv->tv_usec * 1000 };
	struct ml_timer* t = ml_timer_new(ml, &ts, &paml_time_cb);

	struct paml_time_data* td = calloc(1, sizeof(*td));
	td->data = data;
	td->cb = cb;
	td->api = api;
	ml_timer_set_data(t, td);
	ml_timer_set_destroy_db(t, &paml_time_destroy);
	return (pa_time_event*) t;
}

static void paml_time_restart(pa_time_event* e, const struct timeval* tv) {
	if(!tv) {
		ml_timer_restart((struct ml_timer*) e, NULL);
	} else {
		struct timespec ts = {tv->tv_sec, 1000 * tv->tv_usec};
		ml_timer_restart((struct ml_timer*) e, &ts);
	}
}

static void paml_time_free(pa_time_event* e) {
	ml_timer_destroy((struct ml_timer*) e);
}

static void paml_time_set_destroy(pa_time_event* e, pa_time_event_destroy_cb_t cb) {
	struct paml_time_data* td = ml_timer_get_data((struct ml_timer*) e);
	td->destroy_cb = cb;
}

// paml_defer
struct paml_defer_data {
	void* data;
	pa_defer_event_cb_t cb;
	pa_defer_event_destroy_cb_t destroy_cb;
	struct pa_mainloop_api* api;
};

static void paml_defer_cb(struct ml_defer* d) {
	struct paml_defer_data* dd = ml_defer_get_data(d);
	assert(dd->cb);
	dd->cb(dd->api, (pa_defer_event*) d, dd->data);
}

static void paml_defer_destroy(struct ml_defer* d) {
	struct paml_defer_data* dd = ml_defer_get_data(d);
	if(dd->destroy_cb) {
		dd->destroy_cb(dd->api, (pa_defer_event*) d, dd->data);
	}
	free(dd);
}

static pa_defer_event* paml_defer_new(pa_mainloop_api* api,
		pa_defer_event_cb_t cb, void* data) {
	struct mainloop* ml = (struct mainloop*) api->userdata;
	struct ml_defer* d = ml_defer_new(ml, &paml_defer_cb);

	struct paml_defer_data* dd = calloc(1, sizeof(*dd));
	dd->data = data;
	dd->cb = cb;
	dd->api = api;
	ml_defer_set_data(d, dd);
	ml_defer_set_destroy_db(d, &paml_defer_destroy);
	return (pa_defer_event*) d;
}

static void paml_defer_enable(pa_defer_event* e, int enable) {
	ml_defer_enable((struct ml_defer*) e, (bool) enable);
}

static void paml_defer_free(pa_defer_event* e) {
	ml_defer_destroy((struct ml_defer*) e);
}

static void paml_defer_set_destroy(pa_defer_event* e, pa_defer_event_destroy_cb_t cb) {
	struct paml_defer_data* dd = ml_defer_get_data((struct ml_defer*) e);
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

static void pactx_subscribe_cb(pa_context* pactx,
		pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
	printf("pulse event: type = %d, idx = %d\n", t, idx);
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
}

static gboolean player_status_cb(PlayerctlPlayer* player,
		PlayerctlPlaybackStatus status, gpointer data) {
	printf("player changed status to %d\n", (int) status);
	return true;
}

static void glib_prepare(struct ml_custom* c) {
	GMainContext* ctx = ml_custom_get_data(c);
	gint prio;
	g_main_context_prepare(ctx, &prio);
}

static unsigned glib_query(struct ml_custom* c, struct pollfd* fds,
		unsigned n_fds, int* timeout) {
	GPollFD gfds[n_fds + 1];
	const gint prio = INT_MAX;
	GMainContext* ctx = ml_custom_get_data(c);
	unsigned ret = g_main_context_query(ctx, prio, timeout, gfds, n_fds);
	for(unsigned i = 0u; i < n_fds; ++i) {
		fds[i].events = gfds[i].events;
		fds[i].fd = gfds[i].fd;
	}
	return ret;
}

static void glib_dispatch(struct ml_custom* c, struct pollfd* fds, unsigned n_fds) {
	GPollFD gfds[n_fds + 1];
	for(unsigned i = 0u; i < n_fds; ++i) {
		gfds[i].events = fds[i].events;
		gfds[i].revents = fds[i].revents;
		gfds[i].fd = fds[i].fd;
	}
	GMainContext* ctx = ml_custom_get_data(c);
	g_main_context_check(ctx, INT_MAX, gfds, n_fds);
	g_main_context_dispatch(ctx);
}

static const struct ml_custom_impl glib_custom_impl = {
	.prepare = glib_prepare,
	.query = glib_query,
	.dispatch = glib_dispatch
};

int main() {
	struct mainloop* ml = mainloop_new();

	// ml_timer source: triggers once every second
	struct timespec time;
	clock_gettime(CLOCK_REALTIME, &time);
	time.tv_sec += 1;
	struct ml_timer* timer = ml_timer_new(ml, &time, &timer_cb);
	(void) timer;

	// ml_io source: listing for input to stdin
	struct ml_io* io = ml_io_new(ml, STDIN_FILENO, ml_io_input, &fd_cb);
	(void) io;

	// ml_custom source: wrap glib's mainloop to use playerctl
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
	struct ml_custom* glib_custom = ml_custom_new(ml, &glib_custom_impl);
	ml_custom_set_data(glib_custom, gctx);

	// pulseaudio: we use the ml mainloop api to
	struct pa_mainloop_api pa_api = pulse_mainloop_api;
	pa_api.userdata = ml;
	pa_context* pactx = pa_context_new(&pa_api, NULL);
    pa_context_set_state_callback(pactx, pactx_state_cb, NULL);

	if(pa_context_connect(pactx, NULL, 0, NULL) < 0) {
        printf("pa_context_connect() failed: %s", strerror(pa_context_errno(pactx)));
		return 1;
    }

	while(run) {
		mainloop_iterate(ml);
	}

	g_object_unref(manager);
	mainloop_destroy(ml);
}
