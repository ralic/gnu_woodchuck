/* shutdown-monitor.h - Shutdown monitor interface.
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

#ifndef WOODCHUCK_SHUTDOWN_MONITOR_H
#define WOODCHUCK_SHUTDOWN_MONITOR_H

#include <glib-object.h>

#include <stdint.h>

#include "config.h"

/* Shutdown monitor's interface.  */
typedef struct _WCShutdownMonitor WCShutdownMonitor;
typedef struct _WCShutdownMonitorClass WCShutdownMonitorClass;

#define WC_SHUTDOWN_MONITOR_TYPE (wc_shutdown_monitor_get_type ())
#define WC_SHUTDOWN_MONITOR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), WC_SHUTDOWN_MONITOR_TYPE, WCShutdownMonitor))
#define WC_SHUTDOWN_MONITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), WC_SHUTDOWN_MONITOR_TYPE, WCShutdownMonitorClass))
#define IS_WC_SHUTDOWN_MONITOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), WC_SHUTDOWN_MONITOR_TYPE))
#define IS_WC_SHUTDOWN_MONITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), WC_SHUTDOWN_MONITOR_TYPE))
#define WC_SHUTDOWN_MONITOR_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), WC_SHUTDOWN_MONITOR_TYPE, WCShutdownMonitorClass))

struct _WCShutdownMonitorClass
{
  GObjectClass parent;

  /* "shutdown" signal: The system is going to shutdown soon.  Takes
     one parameter: a description, a string.  */
  guint shutdown_signal_id;
};

extern GType wc_shutdown_monitor_get_type (void);

/* Instantiate a shutdown monitor.  */
extern WCShutdownMonitor *wc_shutdown_monitor_new (void);

#endif
