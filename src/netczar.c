#include "config.h"
#include "network-monitor.h"

#include <stdio.h>
#include <glib.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>
#include <sqlite3.h>

#include "debug.h"
#include "util.h"

typedef struct _NetCzar NetCzar;
typedef struct _NetCzarClass NetCzarClass;

#define NETCZAR_TYPE (netczar_get_type ())
#define NETCZAR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), NETCZAR_TYPE, NetCzar))
#define NETCZAR_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), NETCZAR_TYPE, NetCzarClass))
#define IS_NETCZAR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NETCZAR_TYPE))
#define IS_NETCZAR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NETCZAR_TYPE))
#define NETCZAR_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), NETCZAR_TYPE, NetCzarClass))

struct _NetCzarClass
{
  GObjectClass parent;

  DBusGConnection *session_bus;
};

struct _NetCzar
{
  GObject parent;

  NCNetworkMonitor *nm;

  sqlite3 *connectivity_db;
};

extern GType netczar_get_type (void);

/* The server stub file can only be included after declaring the
   prototypes for the callback functions.  */
static gboolean org_walfield_NetCzar_principal_register
  (NetCzar *nc,
   GValueArray *human_readable_name, char *bus_name, GValue *execve,
   char **uuid, GError **error);
static gboolean org_walfield_NetCzar_principal_remove
  (NetCzar *nc, char *uuid, GError **error);
static gboolean org_walfield_NetCzar_job_submit
  (NetCzar *nc,
   char *principal_uuid, char *url, char *location, char *cookie,
   gboolean wakeup, uint64_t trigger_target, uint64_t trigger_earliest,
   uint64_t trigger_latest, uint32_t period, uint32_t request_type,
   uint32_t priority, uint64_t expected_size, char **job_uuid, GError **error);
static gboolean org_walfield_NetCzar_job_evaluate
  (NetCzar *nc,
   char *principal_uuid, uint32_t request_type, uint32_t priority,
   uint64_t expected_size, uint32_t *desirability, GError **error);
static gboolean org_walfield_NetCzar_feedback_subscribe
  (NetCzar *nc, char *principal_uuid, char **handle, GError **error);
static gboolean org_walfield_NetCzar_feedback_unsubscribe
  (NetCzar *nc, char *handle, GError **error);
static gboolean org_walfield_NetCzar_feedback_ack
  (NetCzar *nc, char *job_uuid, uint32_t instance, GError **error);

#include "org.walfield.NetCzar-server.h"

G_DEFINE_TYPE (NetCzar, netczar, G_TYPE_OBJECT);

static void
netczar_class_init (NetCzarClass *klass)
{
  netczar_parent_class = g_type_class_peek_parent (klass);

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
    (NETCZAR_TYPE, &dbus_glib_org_walfield_NetCzar_object_info);
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
netczar_init (NetCzar *nc)
{
  NetCzarClass *klass = NETCZAR_GET_CLASS (nc);

  const char *filename = "connectivity.db";


  /* Initialize the network monitor.  */
  nc->nm = nc_network_monitor_new ();

  g_signal_connect (G_OBJECT (nc->nm), "new-connection",
		    G_CALLBACK (new_connection), nc);
  g_signal_connect (G_OBJECT (nc->nm), "disconnected",
		    G_CALLBACK (disconnected), nc);
  g_signal_connect (G_OBJECT (nc->nm), "default-connection-changed",
		    G_CALLBACK (default_connection_changed), nc);

  g_timeout_add_seconds (5 * 60, connections_dump, nc);

  /* Register with dbus.  */
  dbus_g_connection_register_g_object (klass->session_bus,
				       "/org/walfield/NetCzar",
				       G_OBJECT (nc));

  DBusGProxy *dbus_proxy
    = dbus_g_proxy_new_for_name (klass->session_bus,
				 DBUS_SERVICE_DBUS,
				 DBUS_PATH_DBUS,
				 DBUS_INTERFACE_DBUS);

  char *bus_name = "org.walfield.NetCzar";
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
org_walfield_NetCzar_principal_register
  (NetCzar *nc,
   GValueArray *human_readable_name, char *bus_name, GValue *execve,
   char **uuid, GError **error)
{
  return false;
}

static gboolean
org_walfield_NetCzar_principal_remove
  (NetCzar *nc, char *uuid, GError **error)
{
  return false;
}

static gboolean
org_walfield_NetCzar_job_submit
  (NetCzar *nc,
   char *principal_uuid, char *url, char *location, char *cookie,
   gboolean wakeup, uint64_t trigger_target, uint64_t trigger_earliest,
   uint64_t trigger_latest, uint32_t period, uint32_t request_type,
   uint32_t priority, uint64_t expected_size, char **job_uuid, GError **error)
{
  return false;
}

static gboolean
org_walfield_NetCzar_job_evaluate
  (NetCzar *nc,
   char *principal_uuid, uint32_t request_type, uint32_t priority,
   uint64_t expected_size, uint32_t *desirability, GError **error)
{
  return false;
}

static gboolean
org_walfield_NetCzar_feedback_subscribe
  (NetCzar *nc, char *principal_uuid, char **handle, GError **error)
{
  return false;
}
static gboolean
org_walfield_NetCzar_feedback_unsubscribe
  (NetCzar *nc, char *handle, GError **error)
{
  return false;
}

static gboolean
org_walfield_NetCzar_feedback_ack
  (NetCzar *nc, char *job_uuid, uint32_t instance, GError **error)
{
  return false;
}

int
main (int argc, char *argv[])
{
  g_type_init();
  // dbus_g_thread_init ();

  NetCzar *nc = NETCZAR (g_object_new (NETCZAR_TYPE, NULL));
  if (! nc)
    {
      debug (0, "Failed to allocate memory.");
      abort ();
    }

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  return 0;
}
