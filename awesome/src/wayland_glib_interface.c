#include <stdlib.h>
#include <fcntl.h>
#include <glib.h>
#include <dbus/dbus.h>
#include <wayland-client-core.h>

static GSource *session_source = NULL;
static GSource *system_source = NULL;

void awesome_refresh(void* wayland_state);
gboolean dbus_session_refresh(void* data);
gboolean dbus_system_refresh(void* data);

/* Instance of an event source that we use to integrate the wayland event queue
 * with GLib's MainLoop.
 */
struct InterfaceEventSource {
	GSource source;
	struct wl_display *display;
	void *wayland_state;
	gpointer fd_tag;
};

/* This function is called to prepare polling event source. We just flush
 * and indicate that we have no timeouts, nor are currently pending.
 */
static gboolean interface_prepare(GSource *base, gint *timeout)
{
	struct InterfaceEventSource *interface_source
		= (struct InterfaceEventSource *) base;

	wl_display_flush(interface_source->display);
	*timeout = -1;

	return FALSE;
}

/* This function is called after file descriptors were checked. We indicate that
 * we need to be dispatched if any events on the epoll fd we got from libwayland
 * are pending / need to be handled.
 */
static gboolean interface_check(GSource *base)
{
	struct InterfaceEventSource *interface_source
		= (struct InterfaceEventSource *) base;
	GIOCondition condition
		= g_source_query_unix_fd(base, interface_source->fd_tag);

	/* We need to dispatch if anything happened on the fd */
	return condition != 0;
}

/* This function is called to actually "do" some work. We just run the wayland
 * event queue with a timeout of 0.
 */
static gboolean interface_dispatch(GSource *base, GSourceFunc callback,
		gpointer data)
{
	struct InterfaceEventSource *interface_source
		= (struct InterfaceEventSource *) base;
	if (wl_display_roundtrip(interface_source->display) == -1) {
		exit(0);
	}

	awesome_refresh(interface_source->wayland_state);

	(void) callback;
	(void) data;

	return G_SOURCE_CONTINUE;
}


static void setup_dbus_callback(int fd, GSourceFunc cb, GSource **source) {
	GIOChannel *channel = g_io_channel_unix_new(fd);
	*source = g_io_create_watch(channel, G_IO_IN);
	g_io_channel_unref(channel);
	g_source_set_callback(*source, cb, NULL, NULL);
	g_source_attach(*source, NULL);

	fcntl(fd, F_SETFD, FD_CLOEXEC);
}

void remove_dbus_from_glib() {
	if (session_source) {
		g_source_destroy(session_source);
		session_source = NULL;
	}
	if (system_source) {
		g_source_destroy(system_source);
		system_source = NULL;
	}
}

static GSourceFuncs interface_funcs = {
	.prepare  = interface_prepare,
	.check    = interface_check,
	.dispatch = interface_dispatch,
};

/* Initialise and register an event source with GLib. This event source
 * integrates the wayland event queue with the GLib main loop.
 */
void wayland_glib_interface_init(struct wl_display *display,
		int session_fd, int system_fd, void *wayland_state)
{
	struct InterfaceEventSource *interface_source;
	GSource *source = g_source_new(&interface_funcs, sizeof(*interface_source));

	interface_source = (struct InterfaceEventSource *) source;
	interface_source->wayland_state = wayland_state;
	interface_source->display = display;
	wl_display_roundtrip(interface_source->display);

	interface_source->fd_tag =
		g_source_add_unix_fd(source, wl_display_get_fd(display),
			G_IO_IN | G_IO_ERR | G_IO_HUP);
	g_source_set_can_recurse(source, TRUE);

	setup_dbus_callback(session_fd, dbus_session_refresh, &session_source);
	setup_dbus_callback(system_fd, dbus_system_refresh, &system_source);

	g_source_attach(source, NULL);
}
