/* shutdown-monitor.c - Shutdown monitor.
   Copyright 2011 Neal H. Walfield <neal@walfield.org>

   This file is part of Woodchuck.

   Woodchuck is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   Woodchuck is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "shutdown-monitor.h"

#include <stdio.h>
#include <glib.h>
#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#if HAVE_MAEMO
# include "com.nokia.dsme.signal.h"
#else
# include "org.freedesktop.ConsoleKit.h"
#endif
#include "marshal.h"

#include "debug.h"

struct _WCShutdownMonitor
{
  GObject parent;

  /* System bus.  */
  DBusGConnection *system_bus;

#if HAVE_MAEMO
  /* Proxy object for DSME.
     Service name: com.nokia.dsme,
     Object: /com/nokia/dsme/signal
     Interface: com.nokia.dsme.signal  */
  DBusGProxy *dsme_signal_proxy;
#else
  /* Proxy object for ConsoleKit.
     Service name: org.freedesktop.ConsoleKit,
     Object: /org/freedesktop/ConsoleKit/Manager
     Interface: org.freedesktop.ConsoleKit.Manager  */
  DBusGProxy *consolekit_proxy;
#endif
};

/* The implementation of the shutdown monitor object.  */

static void wc_shutdown_monitor_dispose (GObject *object);

G_DEFINE_TYPE (WCShutdownMonitor, wc_shutdown_monitor, G_TYPE_OBJECT);

static void
wc_shutdown_monitor_class_init (WCShutdownMonitorClass *klass)
{
  wc_shutdown_monitor_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = wc_shutdown_monitor_dispose;

  WCShutdownMonitorClass *sm_class = WC_SHUTDOWN_MONITOR_CLASS (klass);
  sm_class->shutdown_signal_id
    = g_signal_new ("shutdown",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_user_marshal_VOID__STRING,
		    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
shutdown (DBusGProxy *proxy, const char *description, gpointer user_data)
{
  WCShutdownMonitor *m = WC_SHUTDOWN_MONITOR (user_data);

  g_signal_emit (m,
		 WC_SHUTDOWN_MONITOR_GET_CLASS (m)->shutdown_signal_id,
		 0, description);
}

#if !HAVE_MAEMO
static void
console_kit_restart (DBusGProxy *proxy, gpointer user_data)
{
  shutdown (proxy, "restart", user_data);
}

static void
console_kit_stop (DBusGProxy *proxy, gpointer user_data)
{
  shutdown (proxy, "shutdown", user_data);
}
#endif

static void
wc_shutdown_monitor_init (WCShutdownMonitor *m)
{
  GError *error = NULL;
  m->system_bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
  if (error)
    {
      g_critical ("Getting system bus: %s", error->message);
      g_error_free (error);
      return;
    }

#if HAVE_MAEMO
  m->dsme_signal_proxy = dbus_g_proxy_new_for_name
    (m->system_bus,
     "com.nokia.dsme",
     "/com/nokia/dsme/signal",
     "com.nokia.dsme.signal");

  dbus_g_proxy_add_signal (m->dsme_signal_proxy,
			   "shutdown_ind",
			   G_TYPE_STRING,
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (m->dsme_signal_proxy, "shutdown_ind",
			       G_CALLBACK (shutdown), m, NULL);
#else
  m->consolekit_proxy = dbus_g_proxy_new_for_name
    (m->system_bus,
     "org.freedesktop.ConsoleKit",
     "/org/freedesktop/ConsoleKit/Manager",
     "org.freedesktop.ConsoleKit.Manager");

  dbus_g_proxy_add_signal (m->consolekit_proxy,
			   "Restart",
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (m->consolekit_proxy, "Restart",
			       G_CALLBACK (console_kit_restart), m, NULL);

  dbus_g_proxy_add_signal (m->consolekit_proxy,
			   "Stop",
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (m->consolekit_proxy, "Stop",
			       G_CALLBACK (console_kit_stop), m, NULL);
#endif
}

WCShutdownMonitor *
wc_shutdown_monitor_new (void)
{
  /* The user activity monitor is a singleton.  */
  static WCShutdownMonitor *wc_shutdown_monitor;

  /* WC_SHUTDOWN_MONITOR is a singleton.  */
  if (wc_shutdown_monitor)
    g_object_ref (wc_shutdown_monitor);
  else
    wc_shutdown_monitor
      = WC_SHUTDOWN_MONITOR (g_object_new
			     (WC_SHUTDOWN_MONITOR_TYPE, NULL));

  return wc_shutdown_monitor;
}

static void
wc_shutdown_monitor_dispose (GObject *object)
{
  g_critical ("Attempt to dispose user activity singleton!");
}
