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
#include <unistd.h>

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

/* Maps pids to struct wc_process *'s.  */
static GHashTable *pid_to_process;
/* Maps dbus names to struct wc_process *'s.  */
static GHashTable *dbus_name_to_process;

static struct wc_process *
process_lookup (pid_t pid)
{
  return g_hash_table_lookup (pid_to_process, (gpointer) (uintptr_t) pid);
}

static struct wc_process *
process_lookup_by_dbus_name (const char *dbus_name)
{
  return g_hash_table_lookup (dbus_name_to_process, dbus_name);
}

#if 0
static bool
process_info (pid_t pid, char **exep, char **arg0p, char **arg1p)
{
  *exep = *arg0p = *arg1p = NULL;

  char exe[256];
  snprintf (exe, sizeof (exe), "/proc/%d/exe", pid);
  int exe_len = readlink (exe, exe, sizeof (exe) - 1);
  if (exe_len < 0)
    {
      debug (0, "Failed to read link %s: %m", exe);
      exe_len = 0;
    }
  /* Add a NUL terminator.  */
  exe_len ++;
  exe[exe_len - 1] = 0;

  char *arg0 = NULL;
  char *arg1 = NULL;

  char filename[32];
  snprintf (filename, sizeof (filename), "/proc/%d/cmdline", pid);

  gsize length = 0;
  GError *error = NULL;
  if (! g_file_get_contents (filename, &arg0, &length, &error))
    {
      debug (0, "Error reading %s: %s", filename, error->message);
      g_error_free (error);
      error = NULL;

      if (exe_len == 0)
	/* We can read neither the exe nor the arguments.  */
	return false;
    }
  else if (length)
    {
      arg1 = memchr (arg0, 0, length);
      if (! arg1)
	{
	  arg0[length - 1] = 0;
	  arg1 = &arg0[length - 1];
	}
      arg1 ++;

      length -= (uintptr_t) arg1 - (uintptr_t) arg0;
      if (! length)
	arg1 = NULL;
      else
	{
	  char *end = memchr (arg1, 0, length);
	  if (! end)
	    arg1[length - 1] = 0;
	}

      *arg1p = g_strdup (arg1);
    }

  *exep = g_strdup (exe);

  return true;
}
#endif

static struct wc_process *
service_new (WCServiceMonitor *m, pid_t pid, const char *dbus_name)
{
  debug (0, "service_new (%d, %s)", pid, dbus_name);
  assert (dbus_name);

  struct wc_process *process = process_lookup_by_dbus_name (dbus_name);
  if (process)
    {
      debug (0, "Service %s already associated with pid %d "
	     "(you are trying to associate it with %d)",
	     dbus_name, process->pid, pid);

      if (process->pid == pid)
	return process;
      else
	{
	  /* Is the old association stale?  If so, should we move the
	     association?  */
	  assert (0 == 1);
	  return NULL;
	}
    }

  process = process_lookup (pid);
  if (! process)
    /* Not yet tracking this process.  */
    {
      process = g_malloc0 (sizeof (struct wc_process));
      process->pid = pid;
      process->attached = false;

      g_hash_table_insert (pid_to_process,
			   (gpointer) (uintptr_t) pid, process);

      wc_process_monitor_ptrace_trace (process->pid);
    }

  GSList *l;
  for (l = process->dbus_names; l; l = l->next)
    debug (3, "Pid %d also has %s", pid, (char *) l->data);

  /* Add the dbus name to the list of names owned by this process.  */
  char *s = g_strdup (dbus_name);
  process->dbus_names = g_slist_insert_sorted (process->dbus_names, s,
					       (GCompareFunc) g_strcmp0);

  g_hash_table_insert (dbus_name_to_process, s, process);

  if (process->attached)
    g_signal_emit (m,
		   WC_SERVICE_MONITOR_GET_CLASS (m)->service_started_signal_id,
		   0, dbus_name, process);

  return process;
}

static void
service_free (WCServiceMonitor *m,
	      struct wc_process *process, const char *dbus_name)
{
  /* We must remove DBUS_NAME from the hash before we free the
     string.  */
  if (! g_hash_table_remove (dbus_name_to_process, dbus_name))
    {
      debug (0, "Failed to remove %s from dbus_name_to_process hash table.",
	     dbus_name);
      assert (0 == 1);
    }

  assert (process->dbus_names);
  GSList *next = process->dbus_names;
  bool found = false;
  while (next)
    {
      GSList *l = next;
      char *name = l->data;
      next = next->next;

      if (strcmp (name, dbus_name) == 0)
	{
	  process->dbus_names = g_slist_delete_link (process->dbus_names, l);
	  g_free (name);
	  found = true;
	}
      else
	debug (3, "Process %d still provides: %s",
	       process->pid, name);
    }
  assert (found);

  g_signal_emit (m,
		 WC_SERVICE_MONITOR_GET_CLASS (m)->service_stopped_signal_id,
		 0, dbus_name, process);

  if (! process->dbus_names)
    {
      debug (3, "No other services provided by process %d",
	     process->pid);
      if (! g_hash_table_remove (pid_to_process,
				 (gpointer) (uintptr_t) process->pid))
	{
	  debug (0, "Failed to remove %d from pid_to_process hash table",
		 process->pid);
	  assert (0 == 1);
	}

      wc_process_monitor_ptrace_untrace (process->pid);

      g_free (process->exe);
      g_free (process->arg0);
      g_free (process->arg1);

      g_free (process);
    }
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
  if (! arg0)
    /* If we couldn't get arg0, we likely can't trace it.  */
    return false;

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
	  struct wc_process *process = process_lookup_by_dbus_name (name);
	  if (process)
	    service_free (m, process, name);
	}

      if (new_owner && *new_owner)
	{
	  debug (1, "%s assumed %s", new_owner, name);
	  guint pid = 0;
	  if (! (org_freedesktop_DBus_get_connection_unix_process_id
		 (m->dbus_proxy, name, &pid, &error)))
	    {
	      debug (0, "Error fetching pid associated with %s: %s",
		     name, error->message);
	      g_error_free (error);
	      error = NULL;
	    }
	  else
	    {
	      /* /proc/pid/cmdline contains the command line.
		 Arguments are separated by NULs.  */
	      char filename[32];
	      snprintf (filename, sizeof (filename), "/proc/%d/cmdline", pid);

	      gchar *contents = NULL;
	      gsize length = 0;
	      GError *error = NULL;

	      if (! g_file_get_contents (filename, &contents, &length, &error))
		{
		  debug (0, "Error reading %s: %s", filename, error->message);
		  g_error_free (error);
		  error = NULL;
		}
	      else if (length)
		{
		  /* Ensure it is NUL termianted.  */
		  contents[length - 1] = 0;
		  if (! blacklisted_arg0 (contents))
		    service_new (m, pid, name);
		}

	      g_free (contents);
	    }
	}
    }
}

GSList *
wc_service_monitor_list (WCServiceMonitor *m)
{
  GSList *list = NULL;

  void iter (gpointer key, gpointer value, gpointer user_data)
  {
    struct wc_process *process = value;
    if (process->attached)
      {
	GSList *list = *(GSList **) user_data;
	list = g_slist_prepend (list, process);
	*(GSList **) user_data = list;
      }
  }

  g_hash_table_foreach (pid_to_process, iter, &list);

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
  struct wc_process *top_level = process_lookup (cb->top_levels_pid);
  if (! top_level)
    {
      debug (0, "Warning: notification for unmonitored pid %d",
	     cb->top_levels_pid);
      return;
    }

  if (cb->cb == WC_PROCESS_EXIT_CB
      || (cb->cb == WC_PROCESS_TRACING_CB && ! cb->tracing.added))
    {
      /* We can't check TOP_LEVEL->DBUS_NAMES as when we remove the last
	 element, we free TOP_LEVEL.  */
      bool last;
      do
	{
	  last = top_level->dbus_names->next == NULL;
	  service_free (service_monitor, top_level,
			top_level->dbus_names->data);
	}
      while (! last);

      return;
    }

  if (cb->cb == WC_PROCESS_TRACING_CB)
    {
      if (! top_level->attached)
	{
	  top_level->exe
	    = cb->top_levels_exe ? g_strdup (cb->top_levels_exe) : NULL;
	  top_level->arg0
	    = cb->top_levels_arg0 ? g_strdup (cb->top_levels_arg0) : NULL;
	  top_level->arg1
	    = cb->top_levels_arg1 ? g_strdup (cb->top_levels_arg1) : NULL;

	  GSList *l;
	  for (l = top_level->dbus_names; l; l = l->next)
	    g_signal_emit (service_monitor,
			   WC_SERVICE_MONITOR_GET_CLASS (service_monitor)
			   ->service_started_signal_id,
			   0, (char *) l->data, top_level);

	  top_level->attached = true;
	}

      return;
    }

  g_signal_emit (service_monitor,
		 WC_SERVICE_MONITOR_GET_CLASS (service_monitor)
		   ->service_fs_access_signal_id,
		 0, top_level->dbus_names, cb);
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
		    g_cclosure_user_marshal_VOID__STRING_POINTER,
		    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_POINTER);
  pm_class->service_stopped_signal_id
    = g_signal_new ("service-stopped",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_user_marshal_VOID__STRING_POINTER,
		    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_POINTER);
  pm_class->service_fs_access_signal_id
    = g_signal_new ("service-fs-access",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_user_marshal_VOID__POINTER_POINTER,
		    G_TYPE_NONE, 2,
		    G_TYPE_POINTER, G_TYPE_POINTER);
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

  pid_to_process = g_hash_table_new (g_direct_hash, g_direct_equal);
  dbus_name_to_process = g_hash_table_new (g_str_hash, g_str_equal);

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
}

static void
wc_service_monitor_dispose (GObject *object)
{
  g_critical ("Attempt to dispose service monitor singleton!");
}
