/* murmeltier.c - A network and storage manager implementation.
   Copyright (C) 2011 Neal H. Walfield <neal@walfield.org>

   Smart storage is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 3, or (at
   your option) any later version.

   Smart storage is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include "config.h"
#include "network-monitor.h"

#include <stdio.h>
#include <glib.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>
#include <sqlite3.h>

#include "debug.h"
#include "util.h"

typedef struct _Murmeltier Murmeltier;
typedef struct _MurmeltierClass MurmeltierClass;

#define MURMELTIER_TYPE (murmeltier_get_type ())
#define MURMELTIER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), MURMELTIER_TYPE, Murmeltier))
#define MURMELTIER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), MURMELTIER_TYPE, MurmeltierClass))
#define IS_MURMELTIER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MURMELTIER_TYPE))
#define IS_MURMELTIER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), MURMELTIER_TYPE))
#define MURMELTIER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), MURMELTIER_TYPE, MurmeltierClass))

struct _MurmeltierClass
{
  GObjectClass parent;

  DBusGConnection *session_bus;
};

struct _Murmeltier
{
  GObject parent;

  NCNetworkMonitor *nm;

  sqlite3 *connectivity_db;
};

extern GType murmeltier_get_type (void);

/* The server stub file can only be included after declaring the
   prototypes for the callback functions.  */
static gboolean org_woodchuck_server_principal_register
  (Murmeltier *mt,
   GValueArray *human_readable_name, char *bus_name, GValue *execve,
   char **uuid, GError **error);
static gboolean org_woodchuck_server_principal_remove
  (Murmeltier *mt, char *uuid, GError **error);
static gboolean org_woodchuck_server_job_submit
  (Murmeltier *mt,
   char *principal_uuid, char *url, char *location, char *cookie,
   gboolean wakeup, uint64_t trigger_target, uint64_t trigger_earliest,
   uint64_t trigger_latest, uint32_t period, uint32_t request_type,
   uint32_t priority, uint64_t expected_size, char **job_uuid, GError **error);
static gboolean org_woodchuck_server_job_evaluate
  (Murmeltier *mt,
   char *principal_uuid, uint32_t request_type, uint32_t priority,
   uint64_t expected_size, uint32_t *desirability, GError **error);
static gboolean org_woodchuck_server_feedback_subscribe
  (Murmeltier *mt, char *principal_uuid, char **handle, GError **error);
static gboolean org_woodchuck_server_feedback_unsubscribe
  (Murmeltier *mt, char *handle, GError **error);
static gboolean org_woodchuck_server_feedback_ack
  (Murmeltier *mt, char *job_uuid, uint32_t instance, GError **error);

#include "org.woodchuck.server-server.h"

G_DEFINE_TYPE (Murmeltier, murmeltier, G_TYPE_OBJECT);

static void
murmeltier_class_init (MurmeltierClass *klass)
{
  murmeltier_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);

  GError *error = NULL;

  /* Init the DBus connection, per-klass */
  klass->session_bus = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
  if (klass->session_bus == NULL)
    {
      debug (0, "Unable to connect to session bus: %s", error->message);
      g_error_free (error);
      return;
    }

  dbus_g_object_type_install_info
    (MURMELTIER_TYPE, &dbus_glib_org_woodchuck_server_object_info);
}

static void
connection_dump (NCNetworkConnection *nc)
{
  GList *n = nc_network_connection_info (nc, -1);
  while (n)
    {
      GList *e = n;
      n = e->next;

      struct nc_device_info *info = e->data;

      printf ("Interface: %s\n", info->interface);
      char *medium = nc_connection_medium_to_string (info->medium);
      printf ("  Medium: %s\n", medium);
      g_free (medium);
      printf ("  IP: %d.%d.%d.%d\n",
	      info->ip4[0], info->ip4[1], info->ip4[2], info->ip4[3]);
      printf ("  Gateway: %d.%d.%d.%d\n",
	      info->gateway4[0], info->gateway4[1],
	      info->gateway4[2], info->gateway4[3]);
      printf ("  Gateway MAC: %x:%x:%x:%x:%x:%x\n",
	      info->gateway_hwaddr[0],
	      info->gateway_hwaddr[1],
	      info->gateway_hwaddr[2],
	      info->gateway_hwaddr[3],
	      info->gateway_hwaddr[4],
	      info->gateway_hwaddr[5]);
      printf ("  Access point: %s\n", info->access_point);
      printf ("  Stats tx/rx: "BYTES_FMT"/"BYTES_FMT"\n",
	      BYTES_PRINTF (info->stats.tx),
	      BYTES_PRINTF (info->stats.rx));

      g_free (e->data);
      g_list_free_1 (e);
    }
}

static gboolean
connections_dump (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  GList *e = nc_network_monitor_connections (m);
  while (e)
    {
      NCNetworkConnection *c = e->data;
      GList *n = e->next;
      g_list_free_1 (e);
      e = n;

      connection_dump (c);
    }

  return true;
}

/* A new connection has been established.  */
static void
new_connection (NCNetworkMonitor *nm, NCNetworkConnection *nc,
		gpointer user_data)
{
  printf (DEBUG_BOLD ("New %sconnection!!!")"\n",
	  nc_network_connection_is_default (nc) ? "DEFAULT " : "");

  connection_dump (nc);
}

/* An existing connection has been brought down.  */
static void
disconnected (NCNetworkMonitor *nm, NCNetworkConnection *nc,
	      gpointer user_data)
{
  printf ("\nDisconnected!!!\n\n");
}

/* There is a new default connection.  */
static void
default_connection_changed (NCNetworkMonitor *nm,
			    NCNetworkConnection *old_default,
			    NCNetworkConnection *new_default,
			    gpointer user_data)
{
  printf (DEBUG_BOLD ("Default connection changed!!!")"\n");
}

static void
murmeltier_init (Murmeltier *mt)
{
  MurmeltierClass *klass = MURMELTIER_GET_CLASS (mt);

  const char *filename = "connectivity.db";


  /* Initialize the network monitor.  */
  mt->nm = nc_network_monitor_new ();

  g_signal_connect (G_OBJECT (mt->nm), "new-connection",
		    G_CALLBACK (new_connection), mt);
  g_signal_connect (G_OBJECT (mt->nm), "disconnected",
		    G_CALLBACK (disconnected), mt);
  g_signal_connect (G_OBJECT (mt->nm), "default-connection-changed",
		    G_CALLBACK (default_connection_changed), mt);

  g_timeout_add_seconds (5 * 60, connections_dump, mt);

  /* Register with dbus.  */
  dbus_g_connection_register_g_object (klass->session_bus,
				       "/org/woodchuck",
				       G_OBJECT (mt));

  DBusGProxy *dbus_proxy
    = dbus_g_proxy_new_for_name (klass->session_bus,
				 DBUS_SERVICE_DBUS,
				 DBUS_PATH_DBUS,
				 DBUS_INTERFACE_DBUS);

  char *bus_name = "org.woodchuck";
  guint ret;
  GError *error = NULL;
  if (! org_freedesktop_DBus_request_name (dbus_proxy, bus_name,
					   DBUS_NAME_FLAG_DO_NOT_QUEUE, &ret,
					   &error))
    {
      debug (0, DEBUG_BOLD ("Unable to register service: %s"), error->message);
      g_error_free (error);
    }
  switch (ret)
    {
    case DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER:
      debug (0, "Acquired %s", bus_name);
      break;
    case DBUS_REQUEST_NAME_REPLY_IN_QUEUE:
      debug (0, "Waiting for bus name %s to become free", bus_name);
      abort ();
    case DBUS_REQUEST_NAME_REPLY_EXISTS:
      debug (0, "Bus name %s already owned.", bus_name);
      abort ();
    case DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER:
      debug (0, "We already own bus name?");
      abort ();
    default:
      debug (0, "Unknown return code: %d", ret);
      abort ();
    }

  g_object_unref (dbus_proxy);
}

/* The server stub file can only be included after declaring the
   prototypes for the callback functions.  */
static gboolean
org_woodchuck_server_principal_register
  (Murmeltier *mt,
   GValueArray *human_readable_name, char *bus_name, GValue *execve,
   char **uuid, GError **error)
{
  return false;
}

static gboolean
org_woodchuck_server_principal_remove
  (Murmeltier *mt, char *uuid, GError **error)
{
  return false;
}

static gboolean
org_woodchuck_server_job_submit
  (Murmeltier *mt,
   char *principal_uuid, char *url, char *location, char *cookie,
   gboolean wakeup, uint64_t trigger_target, uint64_t trigger_earliest,
   uint64_t trigger_latest, uint32_t period, uint32_t request_type,
   uint32_t priority, uint64_t expected_size, char **job_uuid, GError **error)
{
  return false;
}

static gboolean
org_woodchuck_server_job_evaluate
  (Murmeltier *mt,
   char *principal_uuid, uint32_t request_type, uint32_t priority,
   uint64_t expected_size, uint32_t *desirability, GError **error)
{
  return false;
}

static gboolean
org_woodchuck_server_feedback_subscribe
  (Murmeltier *mt, char *principal_uuid, char **handle, GError **error)
{
  return false;
}
static gboolean
org_woodchuck_server_feedback_unsubscribe
  (Murmeltier *mt, char *handle, GError **error)
{
  return false;
}

static gboolean
org_woodchuck_server_feedback_ack
  (Murmeltier *mt, char *job_uuid, uint32_t instance, GError **error)
{
  return false;
}

int
main (int argc, char *argv[])
{
  g_type_init();
  // dbus_g_thread_init ();

  Murmeltier *mt = MURMELTIER (g_object_new (MURMELTIER_TYPE, NULL));
  if (! mt)
    {
      debug (0, "Failed to allocate memory.");
      abort ();
    }

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  return 0;
}
