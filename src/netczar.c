#include "config.h"
#include "network-monitor.h"

#include <stdio.h>
#include <glib.h>
#include <dbus/dbus-glib.h>

#include "debug.h"
#include "util.h"

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

int
main (int argc, char *argv[])
{
  g_type_init();
  dbus_g_thread_init ();

  NCNetworkMonitor *nm = nc_network_monitor_new ();

  g_signal_connect (G_OBJECT (nm), "new-connection",
		    G_CALLBACK (new_connection), NULL);
  g_signal_connect (G_OBJECT (nm), "disconnected",
		    G_CALLBACK (disconnected), NULL);
  g_signal_connect (G_OBJECT (nm), "default-connection-changed",
		    G_CALLBACK (default_connection_changed), NULL);

  g_timeout_add_seconds (5, connections_dump, nm);

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  return 0;
}
