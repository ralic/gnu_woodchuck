#include "config.h"

#include <stdio.h>
#include <error.h>
#include <glib.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>
#include <sqlite3.h>

#include "debug.h"
#include "util.h"
#include "files.h"
#include "logger-uploader.h"

#include "signal-handler.h"

#include "network-monitor.h"
#include "user-activity-monitor.h"
#include "battery-monitor.h"
#include "service-monitor.h"

/* DB for logging network events.  */
static sqlite3 *nm_db;

static void
nm_connection_dump (NCNetworkConnection *nc)
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
nm_connections_dump (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  GList *e = nc_network_monitor_connections (m);
  while (e)
    {
      NCNetworkConnection *c = e->data;
      GList *n = e->next;
      g_list_free_1 (e);
      e = n;

      nm_connection_dump (c);
    }

  return true;
}

/* A new connection has been established.  */
static void
nm_new_connection (NCNetworkMonitor *nm, NCNetworkConnection *nc,
		   gpointer user_data)
{
  printf (DEBUG_BOLD ("New %sconnection!!!")"\n",
	  nc_network_connection_is_default (nc) ? "DEFAULT " : "");

  nm_connection_dump (nc);
}

/* An existing connection has been brought down.  */
static void
nm_disconnected (NCNetworkMonitor *nm, NCNetworkConnection *nc,
		 gpointer user_data)
{
  printf ("\nDisconnected!!!\n\n");
}

/* There is a new default connection.  */
static void
nm_default_connection_changed (NCNetworkMonitor *nm,
			       NCNetworkConnection *old_default,
			       NCNetworkConnection *new_default,
			       gpointer user_data)
{
  printf (DEBUG_BOLD ("Default connection changed!!!")"\n");
}

static void
nm_init (void)
{
  char *filename = files_logfile ("network.db");

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  char *errmsg = NULL;
  err = sqlite3_exec (db,
		      /* ID is the id of the connection.  It is
			 corresponds to the ROWID in CONNECTIONS.  */
		      "create table if not exists connection_log "
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  year, yday, hour, min, sec,"
		      "  service_type, service_attributes, service_id,"
		      "  network_type, network_attributes, network_id, status,"
		      "  rx, tx);"

		      /* ID is the id of the connection.  It is
			 corresponds to the ROWID in CONNECTIONS.  */
		      "create table if not exists stats_log"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  year, yday, hour, min, sec,"
		      "  service_type, service_attributes, service_id,"
		      "  network_type, network_attributes, network_id,"
		      "  time_active, signal_strength, sent, received);"

		      /* Time that a scan was initiated.  ROWID
			 corresponds to ID in scan_log.  */
		      "create table if not exists scans"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  year, yday, hour, min, sec);"

		      /* ID corresponds to the ROWID of the scans table.  */
		      "create table if not exists scan_log"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT, id,"
		      "  status, last_seen,"
		      "  service_type, service_name, service_attributes,"
		      "	 service_id, service_priority,"
		      "	 network_type, network_name, network_attributes,"
		      "	 network_id, network_priority,"
		      "	 signal_strength, signal_strength_db,"
		      "  station_id);"

		      "create table if not exists cell"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT, "
		      "  year, yday, hour, min, sec,"
		      "  status, lac, cell_id, network, country, "
		      "  network_type, services);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  logger_uploader_table_register (filename, "connection_log", true);
  logger_uploader_table_register (filename, "stats_log", true);
  logger_uploader_table_register (filename, "scans", true);
  logger_uploader_table_register (filename, "scan_log", true);
  logger_uploader_table_register (filename, "cell", true);

  free (filename);

  /* Initialize the network monitor.  */
  NCNetworkMonitor *nm = nc_network_monitor_new ();

  g_signal_connect (G_OBJECT (nm), "new-connection",
		    G_CALLBACK (nm_new_connection), NULL);
  g_signal_connect (G_OBJECT (nm), "disconnected",
		    G_CALLBACK (nm_disconnected), NULL);
  g_signal_connect (G_OBJECT (nm), "default-connection-changed",
		    G_CALLBACK (nm_default_connection_changed), NULL);

  g_timeout_add_seconds (5 * 60, nm_connections_dump, nm);
}

/* User activity monitor.  */
static void
uam_idle_active (WCUserActivityMonitor *m, gboolean idle, int64_t t,
		 gpointer user_data)
{
  debug (0, DEBUG_BOLD ("The user is %s.  Previous state: "TIME_FMT),
	 idle == WC_USER_IDLE ? "idle" : "active", TIME_PRINTF (t));
}

static void
uam_init (void)
{
  /* Initialize the user activity monitor.  */
  WCUserActivityMonitor *m = wc_user_activity_monitor_new ();

  g_signal_connect (G_OBJECT (m), "user-idle-active",
		    G_CALLBACK (uam_idle_active), NULL);
}

static void
battery_status (WCBatteryMonitor *m,
		WCBattery *b,
		int old_is_charging, int is_charging,
		int old_is_discharging, int is_discharging,
		int old_mv, int mv,
		int old_mah, int mah,
		int old_charger, int charger,
		gpointer user_data)
{
  debug (0, "Battery status: "
	 "charging: %d -> %d; discharging: %d -> %d; "
	 "mV: %d -> %d; mAh: %d -> %d; charger: %s -> %s",
	 old_is_charging, is_charging,
	 old_is_discharging, is_discharging,
	 old_mv, mv, old_mah, mah,
	 wc_battery_charger_to_string (old_charger),
	 wc_battery_charger_to_string (charger));
}

static void
bm_init (void)
{
  /* Initialize the battery monitor.  */
  WCBatteryMonitor *m = wc_battery_monitor_new ();

  GSList *batteries = wc_battery_monitor_list (m);
  while (batteries)
    {
      WCBattery *b = WC_BATTERY (batteries->data);

      debug (0, DEBUG_BOLD ("Initial battery status")" %s: "
	     "charging: %d; discharging: %d; "
	     "mV: %d of %d; mAh: %d of %d; charger: %s",
	     wc_battery_id (b),
	     wc_battery_is_charging (b),
	     wc_battery_is_discharging (b),
	     wc_battery_mv (b), wc_battery_mv_design (b),
	     wc_battery_mah (b), wc_battery_mah_design (b),
	     wc_battery_charger_to_string (wc_battery_charger (b)));

      battery_status (m, b,
		      -1, wc_battery_is_charging (b),
		      -1, wc_battery_is_discharging (b),
		      -1, wc_battery_mv (b),
		      -1, wc_battery_mah (b),
		      WC_BATTERY_CHARGER_UNKNOWN, wc_battery_charger (b),
		      NULL);

      g_object_unref (b);
      batteries = g_slist_delete_link (batteries, batteries);
    }

  g_signal_connect (G_OBJECT (m), "battery-status",
		    G_CALLBACK (battery_status), NULL);
}

/* Service monitor.  */

static void
service_started (WCServiceMonitor *m,
		 struct wc_service *service,
		 gpointer user_data)
{
  debug (0, "Service %s started", service->dbus_name);
}

static void
service_stopped (WCServiceMonitor *m,
		 struct wc_service *service,
		 gpointer user_data)
{
  debug (0, "Service %s started", service->dbus_name);
}

static void
service_fs_access (WCServiceMonitor *m,
		   GSList *services,
		   struct wc_process_monitor_cb *cb,
		   gpointer user_data)
{
  char *src = NULL;
  char *dest = NULL;
  struct stat *stat = NULL;

  switch (cb->cb)
    {
    case WC_PROCESS_OPEN_CB:
      src = cb->open.filename;
      stat = &cb->open.stat;
      break;
    case WC_PROCESS_CLOSE_CB:
      src = cb->close.filename;
      stat = &cb->close.stat;
      break;
    case WC_PROCESS_UNLINK_CB:
      src = cb->unlink.filename;
      stat = &cb->unlink.stat;
      break;
    case WC_PROCESS_RENAME_CB:
      src = cb->rename.src;
      dest = cb->rename.dest;
      stat = &cb->unlink.stat;
      break;
    default:
      debug (0, "Unexpected op: %d", cb->cb);
      assert (0 == 1);
      return;
    }

  debug (0, "%d(%d): %s;%s;%s: %s (%s%s%s, "BYTES_FMT")",
	 cb->top_levels_pid, cb->tid,
	 cb->exe, cb->arg0, cb->arg1,
	 wc_process_monitor_cb_str (cb->cb),
	 src, dest ? " -> " : "", dest ?: "",
	 BYTES_PRINTF (stat->st_size));
}

static void
sm_init (void)
{
  WCServiceMonitor *m = wc_service_monitor_new ();

  GSList *services = wc_service_monitor_list (m);
  while ((services))
    {
      struct wc_service *s = services->data;
      debug (0, "  %s is running...", s->dbus_name);

      services = g_slist_delete_link (services, services);
    }

  g_signal_connect (G_OBJECT (m), "service-started",
		    G_CALLBACK (service_started), NULL);
  g_signal_connect (G_OBJECT (m), "service-stopped",
		    G_CALLBACK (service_stopped), NULL);
  g_signal_connect (G_OBJECT (m), "service-fs-access",
		    G_CALLBACK (service_fs_access), NULL);
}

static GMainLoop *loop;

static void
unix_signal_handler (WCSignalHandler *sh, struct signalfd_siginfo *si,
		     gpointer user_data)
{
  debug (0, "Got signal %s.", strsignal (si->ssi_signo));

  if (si->ssi_signo == SIGTERM || si->ssi_signo == SIGINT
      || si->ssi_signo == SIGQUIT || si->ssi_signo == SIGHUP)
    {
      debug (0, "Caught %s, quitting.", strsignal (si->ssi_signo));
      if (loop)
	g_main_loop_quit (loop);
    }
}

static void
signal_handler_init (void)
{
  /* The signals we are iterested in.  */
  sigset_t signal_mask;
  sigemptyset (&signal_mask);
  sigaddset (&signal_mask, SIGTERM);
  sigaddset (&signal_mask, SIGINT);
  sigaddset (&signal_mask, SIGQUIT);
  sigaddset (&signal_mask, SIGHUP);

  WCSignalHandler *sh = wc_signal_handler_new (&signal_mask);

  g_signal_connect (G_OBJECT (sh), "unix-signal",
		    G_CALLBACK (unix_signal_handler), NULL);
}

int
main (int argc, char *argv[])
{
  g_type_init ();
  g_thread_init (NULL);

  /* Initialize the unix signal catcher.  */
  signal_handler_init ();

  /* Initialize each monitor.  */
  nm_init ();
  uam_init ();
  bm_init ();
  sm_init ();

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  return 0;
}
