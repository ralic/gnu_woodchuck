/* service-monitor.c - Service monitor.
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
#include "service-monitor.h"

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

#include "org.freedesktop.DBus.h"

#include "process-monitor-ptrace.h"

#include "marshal.h"

#include "dbus-util.h"
#include "util.h"
#include "debug.h"

struct _WCServiceMonitor
{
  GObject parent;

  /* Session bus.  */
  DBusGConnection *session_bus;

  /* Proxy object for DBus.
     Service name: org.freedesktop.DBus
     Object path: /org/freedesktop/DBus
     Interface: org.freedesktop.DBus  */
  DBusGProxy *dbus_proxy;
};

/* The service monitor service is a singleton.  */
static WCServiceMonitor *service_monitor;

/* The implementation of the service monitor object.  */

/* Maps pids to GSList's of struct wc_service *s.  */
static GHashTable *pid_to_services;
/* Maps a dbus name to struct wc_service *s.  */
static GHashTable *dbus_name_to_service;

static GSList *
services_lookup (pid_t pid)
{
  return g_hash_table_lookup (pid_to_services, (gpointer) (uintptr_t) pid);
}

static struct wc_service *
service_lookup_by_dbus_name (const char *dbus_name)
{
  return g_hash_table_lookup (dbus_name_to_service, dbus_name);
}

static struct wc_service *
service_new (WCServiceMonitor *m,
	     pid_t pid, const char *dbus_name, const char *exe,
	     const char *arg0, const char *arg1)
{
  debug (0, "service_new (%d, %s, %s, %s, %s)",
	 pid, dbus_name, exe, arg0, arg1);

  assert (dbus_name);

  struct wc_service *service = service_lookup_by_dbus_name (dbus_name);
  if (service)
    {
      debug (0, "Service %s already associated with pid %d "
	     "(you are trying to associate it with %d)",
	     dbus_name, service->pid, pid);
      assert (0 == 1);
    }

  int d_len = strlen (dbus_name) + 1;
  int e_len = exe ? strlen (exe) + 1 : 0;
  int a0_len = arg0 ? strlen (arg0) + 1 : 0;
  int a1_len = arg1 ? strlen (arg1) + 1 : 0;
  int len = d_len + e_len + a0_len + a1_len;

  service = g_malloc0 (sizeof (struct wc_service) + len);
  service->pid = pid;

  char *end = mempcpy (service->dbus_name, dbus_name, d_len);
  if (exe)
    {
      service->exe = end;
      end = mempcpy (service->exe, exe, e_len);
    }
  if (arg0)
    {
      service->arg0 = end;
      end = mempcpy (service->arg0, arg0, a0_len);
    }
  if (arg1)
    {
      service->arg1 = end;
      end = mempcpy (service->arg1, arg1, a1_len);
    }

  /* Get the list of services that share this dbus_name (if any).  */
  GSList *pids_services = services_lookup (pid);
  do_debug (3)
    {
      GSList *l;
      for (l = pids_services; l; l = l->next)
	{
	  struct wc_service *service = l->data;
	  debug (0, "Pid %d also has %s",
		 pid, service->dbus_name);
	}
    }

  /* Add the service to the list and update the hash.  */
  pids_services = g_slist_prepend (pids_services, service);
  g_hash_table_insert (pid_to_services,
		       (gpointer) (uintptr_t) pid, pids_services);

  g_hash_table_insert (dbus_name_to_service,
		       service->dbus_name, service);

  if (! pids_services->next)
    /* We are the first service associated with this pid.  */
    wc_process_monitor_ptrace_trace (service->pid);

  g_signal_emit (m, WC_SERVICE_MONITOR_GET_CLASS (m)->service_started_signal_id,
		 0, service);

  return service;
}

/* Returns true if there are no other services associated with PID.  */
static bool
service_free (WCServiceMonitor *m, struct wc_service *service)
{
  GSList *pids_services = services_lookup (service->pid);
  assert (g_slist_find (pids_services, service));

  g_signal_emit (m,
		 WC_SERVICE_MONITOR_GET_CLASS (m)->service_stopped_signal_id,
		 0, service);

  pids_services = g_slist_remove (pids_services, service);

  if (pids_services)
    /* There are other services provided by this pid.  */
    {
      do_debug (3)
	{
	  debug (0, "Not freeing %s (%d).  It still provides:",
		 service->arg0, service->pid);
	  GSList *l;
	  for (l = pids_services; l; l = l->next)
	    {
	      struct wc_service *s = l->data;
	      assert (s->pid == service->pid);
	      debug (0, "%s", s->dbus_name);
	    }
	}
      g_hash_table_insert (pid_to_services,
			   (gpointer) (uintptr_t) service->pid, pids_services);
    }
  else
    {
      debug (3, "No other services provided by %s (%d)",
	     service->arg0, service->pid);
      if (! g_hash_table_remove (pid_to_services,
				 (gpointer) (uintptr_t) service->pid))
	{
	  debug (0, "Failed to remove %s (%d) from hash table?!?",
		 service->dbus_name, service->pid);
	  assert (0 == 1);
	}

      wc_process_monitor_ptrace_untrace (service->pid);
    }

  g_hash_table_remove (dbus_name_to_service, service->dbus_name);

  g_free (service);

  return pids_services != NULL;
}

/* List of binaries that we are not interested in tracing.  The main
   reason is that they don't tend to open interesting files.  Also
   important is that these binaries tend to be critical components,
   which are time sensitive and should never crash.  */
static const char *arg0_blacklist[] =
  {
#if HAVE_MAEMO
    "/usr/bin/hildon-desktop",
    "/usr/bin/hildon-home",
    "/usr/bin/hildon-status-menu",
    "/usr/bin/hildon-input-method",
    "/usr/bin/hildon-sv-notification-daemon",
    "/usr/bin/maemo-xinput-sounds",
    "/usr/bin/profiled",
    "/usr/bin/rtcom-call-ui",
    "/usr/sbin/ohmd",
    "/usr/bin/ohm-session-agent",
    "/usr/bin/mission-control",
    "/usr/lib/telepathy/telepathy-ring",
    "/usr/sbin/alarmd",
    "/usr/sbin/ke-recv",
    "/usr/bin/osso-connectivity-ui-conndlgs",

    /* This doesn't belong here.  But, if we try to play videos it
       won't work.  */
    // "/usr/bin/mafw-dbus-wrapper"
#else
    /* Standard program in a GNOME session we can ignore (at least in
       Debian).  */
    "/usr/bin/pulseaudio",
    "/usr/bin/gnome-terminal",
    "gnome-terminal",
    "gnome-panel",
    "nm-applet",
    "x-session-manager",
    "/usr/lib/libgconf2-4/gconfd-2",
    "gnome-power-manager",
    "/usr/lib/gnome-settings-daemon/gnome-settings-daemon",
    "/usr/bin/gnome-keyring-daemon",
    "/usr/lib/gvfs/gvfs-gdu-volume-monitor",
    "bluetooth-applet",
    "gnome-volume-control-applet",
#endif
  };

static bool
blacklisted_arg0 (const char *arg0)
{
  int i;
  for (i = 0;
       i < sizeof (arg0_blacklist) / sizeof (arg0_blacklist[0]);
       i ++)
    if (strcmp (arg0, arg0_blacklist[i]) == 0)
      {
	debug (3, "Command %s is blacklisted.", arg0);
	return true;
      }

  return false;
}

static void
name_owner_changed_signal_cb (DBusGProxy *proxy,
			      char *name, char *old_owner, char *new_owner,
			      gpointer user_data)
{
  debug (4, "name: %s; old_owner: %s; new_owner: %s",
	 name, old_owner, new_owner);

  WCServiceMonitor *m = WC_SERVICE_MONITOR (user_data);

  GError *error = NULL;

  if (name && name[0] != ':'
      && ((old_owner && *old_owner) || (new_owner && *new_owner)))
    {
      if (old_owner && *old_owner)
	{
	  debug (1, "%s abandoned %s", old_owner, name);

	  struct wc_service *service = service_lookup_by_dbus_name (name);
	  if (service)
	    service_free (m, service);
	}

      if (new_owner && *new_owner)
	{
	  debug (1, "%s assumed %s", new_owner, name);
	  guint pid = 0;
	  char *exe = NULL;
	  char *arg0 = NULL;
	  char *arg1 = NULL;

	  if (org_freedesktop_DBus_get_connection_unix_process_id
	      (m->dbus_proxy, name, &pid, &error))
	    {
	      char filename[32];
	      snprintf (filename, sizeof (filename), "/proc/%d/exe", pid);
	      exe = g_file_read_link (filename, &error);
	      if (! exe)
		{
		  debug (0, "Failed to read %s: %s",
			 filename, error->message);
		  g_error_free (error);
		  error = NULL;
		}

	      snprintf (filename, sizeof (filename), "/proc/%d/cmdline", pid);
	      gsize length = 0;
	      g_file_get_contents (filename, &arg0, &length, &error);
	      if (error)
		{
		  debug (0, "Failed to read %s: %s",
			 filename, error->message);
		  g_error_free (error);
		  error = NULL;
		}
	      else if (arg0 && length)
		{
		  char *arg0end = memchr (arg0, 0, length);
		  if (! arg0end)
		    {
		      char *t = g_malloc (length + 1);
		      memcpy (t, arg0, length);
		      t[length] = 0;
		      g_free (arg0);
		      arg0 = t;
		    }
		  else if (arg0end + 1 < arg0 + length)
		    {
		      arg1 = arg0end + 1;
		      length -= ((uintptr_t) arg1 - (uintptr_t) arg0);
		      char *arg1end = memchr (arg1, 0, length);
		      if (! arg1end && length > 0)
			{
			  /* Stack allocate and then there is no need
			     to free it.  */
			  char *t = alloca (length + 1);
			  memcpy (t, arg1, length);
			  t[length] = 0;
			  arg1 = t;
			}
		    }
		}
	    }
	  else
	    {
	      debug (0, "Error fetching pid associated with %s: %s",
		     name, error->message);
	      g_error_free (error);
	      error = NULL;
	    }

	  /* If we can't read /proc/PID/cmdline, then we probably
	     can't trace the process.  */
	  if (pid && arg0)
	    {
	      if (! blacklisted_arg0 (arg0))
		service_new (m, pid, name, exe, arg0, arg1);
	    }

	  /* There is no need to free arg1.  */
	  g_free (exe);
	  g_free (arg0);
	}
    }
}

GSList *
wc_service_monitor_list (WCServiceMonitor *m)
{
  GSList *list = NULL;

  void iter (gpointer key, gpointer value, gpointer user_data)
  {
    GSList *list = *(GSList **) user_data;
    list = g_slist_prepend (list, value);
    *(GSList **) user_data = list;
  }

  g_hash_table_foreach (dbus_name_to_service, iter, &list);

  return list;
}

WCServiceMonitor *
wc_service_monitor_new (void)
{
  if (service_monitor)
    g_object_ref (service_monitor);
  else
    service_monitor
      = WC_SERVICE_MONITOR (g_object_new (WC_SERVICE_MONITOR_TYPE, NULL));

  return service_monitor;
}

static const char *filename_whitelist[] =
  {
    "/home",
    "/media",
    "/mnt"
  };

bool
process_monitor_filename_whitelisted (const char *filename)
{
  if (! filename)
    return false;

  if (! (filename[0] == '/' && (filename[1] == 'h' || filename[1] == 'm')))
    /* Fast check.  */
    goto out;

  int i;
  for (i = 0;
       i < sizeof (filename_whitelist) / sizeof (filename_whitelist[0]);
       i ++)
    {
      int len = strlen (filename_whitelist[i]);
      if (strncmp (filename, filename_whitelist[i], len) == 0
	  && (filename[len] == 0 || filename[len] == '/'))
	{
	  debug (5, "File %s is whitelisted.", filename);
	  return true;
	}
    }

 out:
  debug (4, "File %s is blacklisted.", filename);

  return false;
}

void
process_monitor_callback (struct wc_process_monitor_cb *cb)
{
  if (cb->cb == WC_PROCESS_EXIT_CB)
    {
      GSList *services = services_lookup (cb->top_levels_pid);
      if (! services)
	{
	  debug (0, "Warning: notification for unmonitored pid %s");
	  return;
	}

      /* We can't iterate over SERVICES as when we remove an element, we
	 change the list.  */
      do
	service_free (service_monitor, services->data);
      while ((services = services_lookup (cb->top_levels_pid)));

      return;
    }

  GSList *services = services_lookup (cb->top_levels_pid);
  if (! services)
    {
      debug (0, "Warning: notification for unmonitored pid %s");
      return;
    }

  g_signal_emit (service_monitor,
		 WC_SERVICE_MONITOR_GET_CLASS (service_monitor)
		   ->service_fs_access_signal_id,
		 0, services, cb);
}

static void wc_service_monitor_dispose (GObject *object);

G_DEFINE_TYPE (WCServiceMonitor, wc_service_monitor, G_TYPE_OBJECT);

static void
wc_service_monitor_class_init (WCServiceMonitorClass *klass)
{
  wc_service_monitor_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = wc_service_monitor_dispose;

  WCServiceMonitorClass *pm_class = WC_SERVICE_MONITOR_CLASS (klass);
  pm_class->service_started_signal_id
    = g_signal_new ("service-started",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_marshal_VOID__POINTER,
		    G_TYPE_NONE, 1, G_TYPE_POINTER);
  pm_class->service_stopped_signal_id
    = g_signal_new ("service-stopped",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_marshal_VOID__POINTER,
		    G_TYPE_NONE, 1, G_TYPE_POINTER);
  pm_class->service_fs_access_signal_id
    = g_signal_new ("service-fs-access",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_user_marshal_VOID__POINTER_POINTER,
		    G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);
}

static void
wc_service_monitor_init (WCServiceMonitor *m)
{
  GError *error = NULL;
  m->session_bus = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
  if (error)
    {
      g_critical ("Getting session bus: %s", error->message);
      g_error_free (error);
      return;
    }

  m->dbus_proxy = dbus_g_proxy_new_for_name
    (m->session_bus,
     "org.freedesktop.DBus",
     "/org/freedesktop/DBus",
     "org.freedesktop.DBus");

  dbus_g_proxy_add_signal (m->dbus_proxy, "NameOwnerChanged",
			   G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, 
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (m->dbus_proxy, "NameOwnerChanged",
			       G_CALLBACK (name_owner_changed_signal_cb),
			       m, NULL);

  pid_to_services = g_hash_table_new (g_direct_hash, g_direct_equal);
  dbus_name_to_service = g_hash_table_new (g_str_hash, g_str_equal);

  /* Start the process monitor.  */
  wc_process_monitor_ptrace_init ();

  /* Get the running services.  */
  char **names = NULL;
  if (! org_freedesktop_DBus_list_names (m->dbus_proxy, &names, &error))
    {
      debug (0, "Failed to call ListNames: %s", error->message);
      g_error_free (error);
    }
  else
    {
      int i;
      for (i = 0; names[i]; i ++)
	{
	  /* Ignore private names.  */
	  if (names[i][0] != ':')
	    name_owner_changed_signal_cb (m->dbus_proxy,
					  names[i], NULL, ":dummy", m);
	  g_free (names[i]);
	}

      g_free (names);
    }

  do_debug (3)
    {
      void iter (gpointer key, gpointer value, gpointer user_data)
      {
	pid_t pid = (pid_t) (uintptr_t) key;

	debug (0, "Pid %d:", pid);

	GSList *l;
	for (l = value; l; l = l->next)
	  {
	    struct wc_service *s = l->data;
	    debug (0, DEBUG_BOLD (" %s;%s;%s;%s"),
		   s->dbus_name, s->exe, s->arg0, s->arg1);
	  }
      }

      debug (0, DEBUG_BOLD ("At start up:"));
      g_hash_table_foreach (pid_to_services, iter, NULL);
    }
}

static void
wc_service_monitor_dispose (GObject *object)
{
  g_critical ("Attempt to dispose service monitor singleton!");
}
