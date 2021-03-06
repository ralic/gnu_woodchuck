/* user-activity-monitor.h - User activity monitor interface.
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

#ifndef WOODCHUCK_USER_ACTIVITY_MONITOR_H
#define WOODCHUCK_USER_ACTIVITY_MONITOR_H

#include <glib-object.h>

#include <stdint.h>

#include "config.h"

/* User activity monitor's interface.  */
typedef struct _WCUserActivityMonitor WCUserActivityMonitor;
typedef struct _WCUserActivityMonitorClass WCUserActivityMonitorClass;

#define WC_USER_ACTIVITY_MONITOR_TYPE (wc_user_activity_monitor_get_type ())
#define WC_USER_ACTIVITY_MONITOR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), WC_USER_ACTIVITY_MONITOR_TYPE, WCUserActivityMonitor))
#define WC_USER_ACTIVITY_MONITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), WC_USER_ACTIVITY_MONITOR_TYPE, WCUserActivityMonitorClass))
#define IS_WC_USER_ACTIVITY_MONITOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), WC_USER_ACTIVITY_MONITOR_TYPE))
#define IS_WC_USER_ACTIVITY_MONITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), WC_USER_ACTIVITY_MONITOR_TYPE))
#define WC_USER_ACTIVITY_MONITOR_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), WC_USER_ACTIVITY_MONITOR_TYPE, WCUserActivityMonitorClass))

enum wc_user_activity_status
  {
    WC_USER_UNKNOWN = 0,
    WC_USER_ACTIVE = 1,
    WC_USER_IDLE = 2
  };

static inline const char *
wc_user_activity_status_string (enum wc_user_activity_status s)
{
  switch (s)
    {
    default:
      return "invalid";
    case WC_USER_UNKNOWN:
      return "unknown";
    case WC_USER_IDLE:
      return "idle";
    case WC_USER_ACTIVE:
      return "active";
    }
}

struct _WCUserActivityMonitorClass
{
  GObjectClass parent;

  /* "user-idle-active" signal: The user has either become idle or
     active.  Takes two parameters: the user's new state (either
     WC_USER_IDLE or WC_USER_ACTIVE), an integer, and the amount of
     time (in milliseconds) spent in the previous state or, if
     unknown, -1, an int64_t.

       void user_idle_active (WCUserActivityMonitor *m,
                              int user_activity_status,
                              int user_activity_status_previous,
			      int64_t time_in_previous_state,
                              gpointer user_data)
  */
  guint idle_active_signal_id;
};

extern GType wc_user_activity_monitor_get_type (void);

/* Return a reference to the idle monitor singleton, instantiating it
   if necessary.  */
extern WCUserActivityMonitor *wc_user_activity_monitor_new (void);

/* Return the user's activity status (idle, active or unknown).  */
extern enum wc_user_activity_status wc_user_activity_monitor_status
  (WCUserActivityMonitor *m);

/* Return the time at which the user became idle or active, in
   milliseconds.  */
extern int64_t wc_user_activity_monitor_status_time_abs
  (WCUserActivityMonitor *m);

/* Return how long the user has been idle or active, in
   milliseconds.  */
extern int64_t wc_user_activity_monitor_status_time
  (WCUserActivityMonitor *m);

#endif
