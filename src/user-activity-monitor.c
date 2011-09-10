/* user-activity-monitor.c - User activity monitor.
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
#include "user-activity-monitor.h"

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
# include "com.nokia.mce.request.h"
# include "com.nokia.mce.signal.h"
#else
# include "org.freedesktop.ConsoleKit.h"
#endif
#include "marshal.h"

#include "dbus-util.h"
#include "util.h"
#include "debug.h"

struct _WCUserActivityMonitor
{
  GObject parent;

  /* System bus.  */
  DBusGConnection *system_bus;

#if HAVE_MAEMO
  /* Proxy object for MCE requests.
     Service name: com.nokia.mce,
     Object: /com/nokia/mce/signal
     Interface: com.nokia.mce.signal  */
  DBusGProxy *mce_request_proxy;
  /* Proxy object for MCE.
     Service name: com.nokia.mce,
     Object: /com/nokia/mce/request
     Interface: com.nokia.mce.signal  */
  DBusGProxy *mce_signal_proxy;
#else
  /* Proxy object for ConsoleKit.
     Service name: org.freedesktop.ConsoleKit,
     Object: /org/freedesktop/ConsoleKit/Manager
     Interface: org.freedesktop.ConsoleKit.Manager  */
  DBusGProxy *consolekit_proxy;
#endif

  /* Whether the user is idle, active or whether this information is
     unknown and at what time the device entered that state (as
     returned by now()).  */
  enum wc_user_activity_status idle;
  int64_t time;
};

/* The implementation of the user activity monitor object.  */

static void wc_user_activity_monitor_dispose (GObject *object);

G_DEFINE_TYPE (WCUserActivityMonitor, wc_user_activity_monitor, G_TYPE_OBJECT);

static void
wc_user_activity_monitor_class_init (WCUserActivityMonitorClass *klass)
{
  wc_user_activity_monitor_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = wc_user_activity_monitor_dispose;

  WCUserActivityMonitorClass *uam_class = WC_USER_ACTIVITY_MONITOR_CLASS (klass);
  uam_class->idle_active_signal_id
    = g_signal_new ("user-idle-active",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_user_marshal_VOID__BOOLEAN_INT64,
		    G_TYPE_NONE, 2, G_TYPE_BOOLEAN, G_TYPE_INT64);
}

static void
idle_changed (DBusGProxy *proxy, gboolean idle, gpointer user_data)
{
  WCUserActivityMonitor *m = WC_USER_ACTIVITY_MONITOR (user_data);

  debug (4, "System idle hint: %s -> %s",
	 wc_user_activity_status_string (m->idle),
	 idle ? "idle" : "active");

  if ((idle && m->idle == WC_USER_IDLE)
      || (! idle && m->idle == WC_USER_ACTIVE))
    {
      debug (5, "Ignoring gratuitous idle hint change.");
      return;
    }

  uint64_t n = now ();

  int64_t time_in_previous_state;
  if (n <= m->time)
    /* Time warp?  */
    time_in_previous_state = -1;
  else
    time_in_previous_state = n - m->time;

  m->time = n;
  m->idle = idle ? WC_USER_IDLE : WC_USER_ACTIVE;

  g_signal_emit (m,
		 WC_USER_ACTIVITY_MONITOR_GET_CLASS (m)
		   ->idle_active_signal_id,
		 0,
		 m->idle, time_in_previous_state);
}

static void
wc_user_activity_refresh (WCUserActivityMonitor *m)
{
  GError *error = NULL;
  gboolean idle = true;
  if (! 
#if HAVE_MAEMO
      com_nokia_mce_request_get_inactivity_status (m->mce_request_proxy,
						   &idle, &error)
#else
      org_freedesktop_ConsoleKit_Manager_get_system_idle_hint
      (m->consolekit_proxy, &idle, &error)
#endif
      )
    {
      debug (0, "Error getting idle hint: %s", error->message);
      g_error_free (error);
      return;
    }

  idle_changed (NULL, idle, m);
}

static void
wc_user_activity_monitor_init (WCUserActivityMonitor *m)
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
  m->mce_signal_proxy = dbus_g_proxy_new_for_name
    (m->system_bus,
     "com.nokia.mce",
     "/com/nokia/mce/signal",
     "com.nokia.mce.signal");

  dbus_g_proxy_add_signal (m->mce_signal_proxy,
			   "system_inactivity_ind",
			   G_TYPE_BOOLEAN,
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (m->mce_signal_proxy, "system_inactivity_ind",
			       G_CALLBACK (idle_changed), m, NULL);

  m->mce_request_proxy = dbus_g_proxy_new_from_proxy
    (m->mce_signal_proxy,
     "com.nokia.mce.request",
     "/com/nokia/mce/request");
#else
  m->consolekit_proxy = dbus_g_proxy_new_for_name
    (m->system_bus,
     "org.freedesktop.ConsoleKit",
     "/org/freedesktop/ConsoleKit/Manager",
     "org.freedesktop.ConsoleKit.Manager");

  dbus_g_proxy_add_signal (m->consolekit_proxy,
			   "SystemIdleHintChanged",
			   G_TYPE_BOOLEAN,
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (m->consolekit_proxy, "SystemIdleHintChanged",
			       G_CALLBACK (idle_changed), m, NULL);
#endif

  m->idle = WC_USER_UNKNOWN;
  m->time = now ();
  wc_user_activity_refresh (m);
}

WCUserActivityMonitor *
wc_user_activity_monitor_new (void)
{
  /* The user activity monitor is a singleton.  */
  static WCUserActivityMonitor *wc_user_activity_monitor;

  /* WC_USER_ACTIVITY_MONITOR is a singleton.  */
  if (wc_user_activity_monitor)
    g_object_ref (wc_user_activity_monitor);
  else
    wc_user_activity_monitor
      = WC_USER_ACTIVITY_MONITOR (g_object_new
				  (WC_USER_ACTIVITY_MONITOR_TYPE, NULL));

  return wc_user_activity_monitor;
}

static void
wc_user_activity_monitor_dispose (GObject *object)
{
  g_critical ("Attempt to dispose user activity singleton!");
}

enum wc_user_activity_status
wc_user_activity_monitor_status (WCUserActivityMonitor *m)
{
  return m->idle;
}

int64_t
wc_user_activity_monitor_status_time_abs (WCUserActivityMonitor *m)
{
  uint64_t n = now ();

  if (n <= m->time)
    /* Time warp?  */
    m->time = n;
  return m->time;
}

int64_t
wc_user_activity_monitor_status_time (WCUserActivityMonitor *m)
{
  return now () - wc_user_activity_monitor_status_time_abs (m);
}
