/* service-monitor.h - Service monitor interface.
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

#ifndef WOODCHUCK_SERVICE_MONITOR_H
#define WOODCHUCK_SERVICE_MONITOR_H

#include <glib-object.h>

#include <stdint.h>

#include "config.h"

#include "process-monitor-ptrace.h"

/* Service interface.  */

struct wc_process
{
  pid_t pid;

  /* Set to true when the process tracer backend confirms that we are
     tracing the process.  Until then, no service-started signals are
     sent.  */
  bool attached;

  /* The dbus names that this service owns.  List of char *.  This
     list is sorted.  */
  GSList *dbus_names;

  char *exe;
  char *arg0;
  char *arg1;
};

/* Service monitor's interface.  */
typedef struct _WCServiceMonitor WCServiceMonitor;
typedef struct _WCServiceMonitorClass WCServiceMonitorClass;

#define WC_SERVICE_MONITOR_TYPE (wc_service_monitor_get_type ())
#define WC_SERVICE_MONITOR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), WC_SERVICE_MONITOR_TYPE, WCServiceMonitor))
#define WC_SERVICE_MONITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), WC_SERVICE_MONITOR_TYPE, WCServiceMonitorClass))
#define IS_WC_SERVICE_MONITOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), WC_SERVICE_MONITOR_TYPE))
#define IS_WC_SERVICE_MONITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), WC_SERVICE_MONITOR_TYPE))
#define WC_SERVICE_MONITOR_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), WC_SERVICE_MONITOR_TYPE, WCServiceMonitorClass))

struct _WCServiceMonitorClass
{
  GObjectClass parent;

  /* "service-started" signal: Called when a service starts.  Takes 2
     parameters: the dbus name (a const char *) and a process
     descriptor (struct wc_process *).  */
  guint service_started_signal_id;
  /* "service-started" signal: Called when a service stops.  Takes the
     same arguments as the "service-started" signal.  */
  guint service_stopped_signal_id;

  /* "service-fs-access" signal: Called when a service (or a
     sub-process) opens, closes, unlinks or renames a file.  Passed a
     list of service names registered by the top level process (a
     sorted GSList * of const char *) and a struct
     wc_process_monitor_cb * describing the action.  */
  guint service_fs_access_signal_id;
};

extern GType wc_service_monitor_get_type (void);

/* Instantiate a idle monitor.  After instantiating, immediately
   connect to the object's "idle" and "disconnected" signals: existing
   connections will be created at the next "idle" point.  */
extern WCServiceMonitor *wc_service_monitor_new (void);

/* List the currently running services.  Returns a list struct
   wc_process's.  The caller must free the returned list (but, not the
   elements!):

     g_slist_free (list);
*/
extern GSList *wc_service_monitor_list (WCServiceMonitor *m);

#endif
