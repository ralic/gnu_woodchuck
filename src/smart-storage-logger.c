/* smart-storage-logger.c - Smart storage logger.
   Copyright (C) 2009, 2010, 2011 Neal H. Walfield <neal@walfield.org>

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

#include <stdio.h>
#include <error.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>
#include <sqlite3.h>
#include <sqlq.h>

#include "debug.h"
#include "util.h"
#include "files.h"
#include "pidfile.h"
#include "smart-storage-logger-uploader.h"

#include "signal-handler.h"

#include "network-monitor.h"
#include "user-activity-monitor.h"
#include "battery-monitor.h"
#include "service-monitor.h"
#include "shutdown-monitor.h"

/* DB for logging events.  */
static char *db_filename;
static sqlite3 *db;

char sqlq_buffer[64 * 4096];
struct sqlq *sqlq;

#define SQL_TIME_COLS "year, yday, hour, min, sec"
#define TM_FMT "%d, %d, %d, %d, %d"
#define TM_PRINTF(tm) (1900 + (tm).tm_year), (tm).tm_yday, (tm).tm_hour, \
    (tm).tm_min, (tm).tm_sec

static NCNetworkMonitor *nm;

static uint64_t nm_stop_logging;

/* The time, as returned by now, of the last network scan.  */
static uint64_t last_scan;

/* How often to scan for networks, in ms.  */
#define SCAN_INTERVAL_MAX (3 * 60 * 60 * 1000)
/* We must wait at least this long before performing a scan.  */
#define SCAN_INTERVAL_MIN (60 * 60 * 1000)

static void
nm_scan_queue (void)
{
  uint64_t n = now ();
  if (now () - last_scan >= SCAN_INTERVAL_MIN)
    {
      last_scan = n;
      nm_scan (nm);
    }
}

static void
nm_connection_dump (NCNetworkConnection *nc, const char *state)
{
  /* We want to save NC's configuration and current statistics.

     A connection consists of a series of configurated devices (but
     typically just one).  The devices' configuration can change over
     time.  For instance, the IP address may change.

     To this end, we have three tables. 

       - device_configuration stores a list of device configurations.

       - connection_configuration stores a list of connection
         configurations, which consist of up to 4 device
         configurations.

       - connection_stats stores a snapshot of connections, which
         includes the current configuration and the number of bytes
         transferred over each configured device.

     We need to:

       - insert each device's configuration into the device_configuration
         table

       - insert the connection's configuration into the
         connection_configuration table

       - snapshot the connection's state.

     We do all three of these things in parallel to reduce the amount
     of string processing required.  */

  if (nm_stop_logging)
    /* We are shutting down.  */
    {
      uint64_t t = now () - nm_stop_logging;
      if (t > 2000)
	/* We started shutting down more than 2 seconds ago.
	   Complain.  */
	debug (0, "Ignoring %s log request from "TIME_FMT" ago",
	       state, TIME_PRINTF (t));
      return;
    }


  /* Add an item to a list of items.  Prefix it with SEP if it is not
     the first item (as indicated by *HAVE_ONE).  */
  void item (GString *s, bool *have_one, const char *sep, const char *fmt, ...)
  {
    va_list ap;
    va_start (ap, fmt);

    if (*have_one)
      g_string_append_printf (s, sep);
    *have_one = true;

    g_string_append_vprintf (s, fmt, ap);

    va_end (ap);
  }

  GList *devices = nc_network_connection_info (nc, -1);
  if (! devices)
    {
      debug (0, "Connection %s has no associated devices.",
	     nc_network_connection_id (nc));
      return;
    }

  /* The SQL for the connection's configuration looks like:

       insert or ignore into connection_configuration (DID1, DID2, DID3, DID4)
         values (# From DIDS->STR
                 (select OID from device_configuration where AP = '...' ...),
	         ...
		);
   */
  GString *dids = g_string_sized_new (1024);
  bool dids_have_one = false;
  /* The connection_configuration has space for at most 4 devices.  */
#define DIDS 4
  int did_count = 0;

  /* As we process each element of the list, we deallocate it.  We
     need the time when the stats were collected.  The time is the
     same for all devices so just stash the first one's time
     stamp.  */
  uint64_t t_ms = ((struct nc_device_info *) devices->data)->stats.time;


  /* The SQL for connection statistics looks like this:

      insert into connection_stats
        (SQL_TIME_COLS, CID, connection_configuration,
	 tx1, rx1, ..., tx4, rx4, time, state, default_route)
        values (TM_PRINTF(), connection_id,
                (select OID from connection_configuration
		 where
		    # From CC->STR
	                DID1 = (select OID from device_configuration
		                  where IP = 'a.b.c.d' ...)
		    and DID2 = (select OID from device_configuration
		                  where IP = 'a.b.c.d' ...)),
                STATS->STR, ...);
  */
  GString *cc = g_string_sized_new (1024);
  bool cc_have_one = false;

  GString *stats = g_string_sized_new (96);
  bool stats_have_one = false;

  struct nc_device_info *d = NULL;
  while (1)
    {
      if (d)
	{
	  g_free (devices->data);
	  GList *n = devices->next;
	  g_list_free_1 (devices);
	  devices = n;
	}
      if (! devices)
	/* We're done.  */
	break;
      if (++ did_count > DIDS)
	/* There are more devices then there is space in the table.
	   Skip it.  */
	continue;

      d = devices->data;

#if 0
      printf ("%s: Interface: %s\n",
	      nc_network_connection_id (nc), d->interface);
      char *medium = nc_connection_medium_to_string (d->medium);
      printf ("  Medium: %s\n", medium);
      g_free (medium);
      printf ("  IP: %d.%d.%d.%d\n",
	      d->ip4[0], d->ip4[1], d->ip4[2], d->ip4[3]);
      printf ("  Gateway: %d.%d.%d.%d\n",
	      d->gateway4[0], d->gateway4[1],
	      d->gateway4[2], d->gateway4[3]);
      printf ("  Gateway MAC: %x:%x:%x:%x:%x:%x\n",
	      d->gateway_hwaddr[0],
	      d->gateway_hwaddr[1],
	      d->gateway_hwaddr[2],
	      d->gateway_hwaddr[3],
	      d->gateway_hwaddr[4],
	      d->gateway_hwaddr[5]);
      printf ("  Access point: %s\n", d->access_point);
      printf ("  Stats tx/rx: "BYTES_FMT"/"BYTES_FMT"\n",
	      BYTES_PRINTF (d->stats.tx),
	      BYTES_PRINTF (d->stats.rx));
#endif

      item (dids, &dids_have_one, ",",
	    "(select OID from device_configuration where ");

      g_string_append_printf
	(cc,
	 "%sDID%d = (select OID from device_configuration where ",
	 cc_have_one ? "and " : "", did_count);
      cc_have_one = true;

      /* The SQL for inserting the device configuration looks like:

	   insert or ignore into device_configuration
	     (IP, AP, ...) values ('1.2.3.4', 'foo', ...);

	   We build the required columns and values in parallel and
	   then paste them together.

	   At the same time, we build up the select part for the
	   connection configuration and connection stats sql.
       */
      GString *c = g_string_sized_new (1024);
      GString *v = g_string_sized_new (1024);
      int val_count = 0;

      void col (const char *fmt, ...)
      {
	va_list ap;
	va_start (ap, fmt);

	if (val_count >= 1)
	  {
	    g_string_append_printf (c, ",");
	    g_string_append_printf (dids, " and ");
	    g_string_append_printf (cc, " and ");
	  }

	g_string_append_vprintf (c, fmt, ap);

	g_string_append_vprintf (dids, fmt, ap);
	g_string_append_printf (dids, "=");

	g_string_append_vprintf (cc, fmt, ap);
	g_string_append_printf (cc, "=");

	va_end (ap);
      }
      void val (const char *fmt, ...)
      {
	va_list ap;
	va_start (ap, fmt);

	if (val_count >= 1)
	  g_string_append_printf (v, ",");

	char *t = sqlite3_vmprintf (fmt, ap);

	g_string_append_printf (v, t);
	g_string_append_printf (dids, t);
	g_string_append_printf (cc, t);

	sqlite3_free (t);

	va_end (ap);

	val_count ++;
      }

#define DEFAULT_VALUE "'NONE'"
      col ("IFACE");
      if ((d->mask & NC_DEVICE_INFO_INTERFACE))
	val ("'%q'", d->interface);
      else
	val (DEFAULT_VALUE);

      col ("MEDIUM");
      if ((d->mask & NC_DEVICE_INFO_MEDIUM))
	{
	  char *medium = nc_connection_medium_to_string (d->medium);
	  val ("'%q (%d)'", medium, d->medium);
	  g_free (medium);
	}
      else
	val (DEFAULT_VALUE);

      col ("IP4");
      if ((d->mask & NC_DEVICE_INFO_IP_IP4_ADDR))
	{
	  val ("'%d.%d.%d.%d'",
		d->ip4[0], d->ip4[1], d->ip4[2], d->ip4[3]);
	}
      else
	val (DEFAULT_VALUE);

      col ("IP6");
      if ((d->mask & NC_DEVICE_INFO_IP_IP6_ADDR))
	val ("'%02x%02x:%02x%02x:"
	     "%02x%02x:%02x%02x:"
	     "%02x%02x:%02x%02x:"
	     "%02x%02x:%02x%02x'",
	     d->ip6[0], d->ip6[1], d->ip6[2], d->ip6[3],
	     d->ip6[4], d->ip6[5], d->ip6[6], d->ip6[7],
	     d->ip6[8], d->ip6[9], d->ip6[10], d->ip6[11],
	     d->ip6[12], d->ip6[13], d->ip6[14], d->ip6[15]);
      else
	val (DEFAULT_VALUE);

      col ("GW4");
      if ((d->mask & NC_DEVICE_INFO_GATEWAY_IP4_ADDR))
	val ("'%d.%d.%d.%d'",
	     d->gateway4[0], d->gateway4[1], d->gateway4[2], d->gateway4[3]);
      else
	val (DEFAULT_VALUE);

      col ("GW6");
      if ((d->mask & NC_DEVICE_INFO_GATEWAY_IP6_ADDR))
	val ("'%02x%02x:%02x%02x:"
	     "%02x%02x:%02x%02x:"
	     "%02x%02x:%02x%02x:"
	     "%02x%02x:%02x%02x'",
	     d->gateway6[0], d->gateway6[1],
	     d->gateway6[2], d->gateway6[3],
	     d->gateway6[4], d->gateway6[5],
	     d->gateway6[6], d->gateway6[7],
	     d->gateway6[8], d->gateway6[9],
	     d->gateway6[10], d->gateway6[11],
	     d->gateway6[12], d->gateway6[13],
	     d->gateway6[14], d->gateway6[15]);
      else
	val (DEFAULT_VALUE);

      col ("GWMAC");
      if ((d->mask & NC_DEVICE_INFO_GATEWAY_MAC_ADDR))
	val ("'%02x:%02x:%02x:%02x:%02x:%02x'",
	     d->gateway_hwaddr[0], d->gateway_hwaddr[1],
	     d->gateway_hwaddr[2], d->gateway_hwaddr[3],
	     d->gateway_hwaddr[4], d->gateway_hwaddr[5]);
      else
	val (DEFAULT_VALUE);

      col ("AP");
      if ((d->mask & NC_DEVICE_INFO_ACCESS_POINT))
	val ("'%q'", d->access_point);
      else
	val (DEFAULT_VALUE);

      g_string_append_printf (dids, ")");
      g_string_append_printf (cc, ")");

      sqlq_append_printf (sqlq, false,
			  "insert or ignore into device_configuration"
			  " (%s) values (%s);",
			  c->str, v->str);
      g_string_free (c, true);
      g_string_free (v, true);

      item (stats, &stats_have_one, ",",
	    "%"PRId64",%"PRId64, d->stats.tx, d->stats.rx);
    }
  for (; did_count < DIDS; did_count ++)
    /* Add default values for the rest of the device slots.  */
    {
      item (dids, &dids_have_one, ",", DEFAULT_VALUE);
      item (cc, &cc_have_one, " and ", "DID%d = "DEFAULT_VALUE,
	    did_count + 1);
      item (stats, &stats_have_one, ",", "0, 0");
    }

  sqlq_append_printf (sqlq, false,
		      "insert or ignore into connection_configuration"
		      " (DID1, DID2, DID3, DID4) values (%s);",
		      dids->str);
  g_string_free (dids, true);

  time_t t = t_ms / 1000;
  struct tm tm;
  localtime_r (&t, &tm);

  sqlq_append_printf
    (sqlq, false,
     "insert into connection_stats"
     " ("SQL_TIME_COLS", CID, connection_configuration,"
     "  tx1, rx1, tx2, rx2, tx3, rx3, tx4, rx4, "
     "  time, state, default_route)"
     " values ("TM_FMT", '%q',"
     "  (select OID from connection_configuration where %s),"
     "  %s, %"PRId64", '%s', '%s');",
     TM_PRINTF (tm), nc_network_connection_id (nc), cc->str, stats->str,
     (uint64_t) (t_ms - nc_network_connection_time_established (nc)),
     state, nc_network_connection_is_default (nc) ? "default" : "");
  g_string_free (cc, true);
  g_string_free (stats, true);
}

static void
nm_connections_dump (NCNetworkMonitor *m, const char *state)
{
  GList *e = nc_network_monitor_connections (m);
  while (e)
    {
      NCNetworkConnection *c = e->data;
      GList *n = e->next;
      g_list_free_1 (e);
      e = n;

      nm_connection_dump (c, state);
    }
}

static gboolean
nm_connections_stat_cb (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);
  nm_connections_dump (m, "STATS");

  if (now () - last_scan >= SCAN_INTERVAL_MAX)
    nm_scan_queue ();

  return true;
}

/* A new connection has been established.  */
static void
nm_new_connection (NCNetworkMonitor *nm, NCNetworkConnection *nc,
		   gpointer user_data)
{
  nm_connection_dump (nc, "ESTABLISHED");
}

/* An existing connection has been brought down.  */
static void
nm_disconnected (NCNetworkMonitor *nm, NCNetworkConnection *nc,
		 gpointer user_data)
{
  nm_connection_dump (nc, "DISCONNECTED");
}

/* There is a new default connection.  */
static void
nm_default_connection_changed (NCNetworkMonitor *nm,
			       NCNetworkConnection *old_default,
			       NCNetworkConnection *new_default,
			       gpointer user_data)
{
  if (old_default)
    nm_connection_dump (old_default, "STATS");
  if (new_default)
    nm_connection_dump (new_default, "STATS");
}

/* Network scan results are available.  */
static void
nm_scan_results (NCNetworkMonitor *nm, GSList *aps, gpointer user_data)
{
  struct tm tm = now_tm ();

  sqlq_append_printf (sqlq, false,
		      "insert into access_point_scan"
		      " ("SQL_TIME_COLS", network_type)"
		      " values ("TM_FMT", %Q);",
		      TM_PRINTF (tm),
		      (aps ? ((struct nm_ap *) aps->data)->network_type
		       : "UNKNOWN"));

  for (; aps; aps = aps->next)
    {
      struct nm_ap *ap = aps->data;

      debug (4, DEBUG_BOLD ("Station")": "
	     "%s;%s;%s: %s, signal %d (%d dB), flags: %"PRIx32,
	     ap->user_id, ap->station_id, ap->network_id,
	     ap->network_type, ap->signal_strength_normalized,
	     ap->signal_strength_db, ap->network_flags);

      sqlq_append_printf
	(sqlq, true,
	 "insert or ignore into access_point"
	 " (user_id, station_id, network_id, network_type)"
	 " values (%Q, %Q, %Q, %Q);"
	 "insert into access_point_log"
	 " (APSID, APID, flags,"
	 "  signal_strength_normalized, signal_strength_db)"
	 " values ((select MAX (OID) from access_point_scan),"
	 "  (select OID from access_point where"
	 "    user_id=%Q and station_id=%Q and network_id=%Q"
	 "    and network_type=%Q),"
	 "  '%"PRIx32"', %d, %d);",
	 ap->user_id ?: DEFAULT_VALUE, ap->station_id ?: DEFAULT_VALUE,
	 ap->network_id ?: DEFAULT_VALUE, ap->network_type ?: DEFAULT_VALUE,
	 ap->user_id ?: DEFAULT_VALUE, ap->station_id ?: DEFAULT_VALUE,
	 ap->network_id ?: DEFAULT_VALUE, ap->network_type ?: DEFAULT_VALUE,
	 ap->network_flags, ap->signal_strength_normalized,
	 ap->signal_strength_db);
    }
}

static void
nm_cell_info_changed (NCNetworkMonitor *nm, GSList *cells, gpointer user_data)
{
  struct tm tm = now_tm ();

  GSList *l;
  for (l = cells; l; l = l->next)
    {
      struct nm_cell *c = l->data;
#define X_(a, b) a ## b
#define X(flag, field)							\
      (X_(NM_CELL_,flag) & c->changes) ? DEBUG_BOLD_BEGIN : "",		\
	c->field,							\
	(X_(NM_CELL_,flag) & c->changes) ? DEBUG_BOLD_END : ""
      
      debug (4, "cell info: %sconnected: %d%s;"
	     " %sLAC: %"PRId16"%s;"
	     " %scell id: %"PRId32"%s;"
	     " %snetwork: %"PRId32"%s;"
	     " %scountry: %"PRId32"%s;"
	     " %snetwork type: %d%s;"
	     " %ssignal strength normalized: %d%s;"
	     " %ssignal strength dbm: %d%s;"
	     " %soperator: %s%s",
	     X(CONNECTED, connected),
	     X(LAC, lac),
	     X(CELL_ID, cell_id),
	     X(NETWORK, network),
	     X(COUNTRY, country),
	     X(NETWORK_TYPE, network_type),
	     X(SIGNAL_STRENGTH_NORMALIZED, signal_strength_normalized),
	     X(SIGNAL_STRENGTH_DBM, signal_strength_dbm),
	     X(OPERATOR, operator));
#undef X_
#undef X

      sqlq_append_printf
	(sqlq, false,
	 "insert or ignore into cells"
	 " (lac, cell_id, network, country, network_type, operator)"
	 " values (%"PRId16", %"PRId32", %"PRId32","
	 "  %"PRId32", %d, '%q');"

	 "insert into cell_info"
	 " ("SQL_TIME_COLS", cell_id, connected, signal_strength_normalized, "
	 "  signal_strength_dbm)"
	 " values"
	 " ("TM_FMT","
	 "  (select OID from cells"
	 "    where lac = %"PRId16" and cell_id = %"PRId32
	 "     and network = %"PRId32" and country = %"PRId32
	 "     and network_type = %d and operator = '%q'),"
	 "  '%s', %d, %d);",
	 c->lac, c->cell_id, c->network, c->country, c->network_type,
	 c->operator,

	 TM_PRINTF (tm), c->lac, c->cell_id, c->network, c->country,
	 c->network_type, c->operator, c->connected ? "connected" : "neighbor",
	 c->signal_strength_normalized, c->signal_strength_dbm);
    }
}

static void
nm_init (void)
{
  char *errmsg = NULL;
  int err;
  err = sqlite3_exec (db,
		      /* List of known connections.  A connection is a
			 collection of device configurations.  The
			 first device tunnels data into the second,
			 etc.  For instance, a VPN may tunnel data
			 over an ethernet connection.  DIDX is the
			 device id of the device configuration in the
			 DEVICE_CONFIGURATION table.  Most connections
			 will only use a single device.  Any unused
			 slots should be filled in with the string
			 NONE, not NULL because we want (X, NULL, ...)
			 to match (X, NULL, ...).  */
		      "create table if not exists connection_configuration "
		      "(OID INTEGER PRIMARY KEY AUTOINCREMENT, "
		      " DID1 NOT NULL DEFAULT 'NONE',"
		      " DID2 NOT NULL DEFAULT 'NONE',"
		      " DID3 NOT NULL DEFAULT 'NONE',"
		      " DID4 NOT NULL DEFAULT 'NONE',"
		      " UNIQUE (DID1, DID2, DID3, DID4));"
		      "create index if not exists"
		      " connection_configuration_index"
		      " on connection_configuration"
		      " (DID1, DID2);"

		      /* List of known device configurations.  AP is
			 the access point for WiFi, the network
			 operator for GSM.  If some information is not
			 available, provide the string NONE.  */
		      "create table if not exists device_configuration"
		      "(OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      " IFACE NOT NULL DEFAULT 'NONE',"
		      " MEDIUM NOT NULL DEFAULT 'NONE',"
		      " IP4 NOT NULL DEFAULT 'NONE',"
		      " IP6 NOT NULL DEFAULT 'NONE',"
		      " GW4 NOT NULL DEFAULT 'NONE',"
		      " GW6 NOT NULL DEFAULT 'NONE',"
		      " GWMAC NOT NULL DEFAULT 'NONE',"
		      " AP NOT NULL DEFAULT 'NONE',"
		      " UNIQUE (IFACE, MEDIUM, IP4, IP6, GW4, GW6, GWMAC, AP)"
                      ");"
		      "create index if not exists device_configuration_index"
		      " on device_configuration"
		      " (IFACE, MEDIUM, IP4, IP6, GW4, GW6, GWMAC, AP);"

		      /* CID is the connection's stable identifier.
			 CONNECTION_CONFIGURATION is the OID of the
			 connection_configuration in the CONNECTION
			 CONFIGURATION table.  rx and tx are in bytes.
			 TIME is the amount of time the connection has
			 been established in milliseconds.  STATE is
			 the STATE of the connection: "ESTABLISHED",
			 "STATS", "DISCONNECTED".  DEFAULT is whether
			 the connection is the default route
			 ("default" or "").  */
		      "create table if not exists connection_stats "
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  "SQL_TIME_COLS", CID, connection_configuration, "
		      "  rx1, tx1, rx2, tx2, rx3, tx3, rx4, tx4,"
		      "  time, state, default_route);"

		      /* List of access points that we have seen.  */
		      "create table if not exists access_point"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  user_id NOT NULL DEFAULT 'NONE',"
		      "  station_id NOT NULL DEFAULT 'NONE',"
		      "  network_id NOT NULL DEFAULT 'NONE',"
		      "  network_type NOT NULL DEFAULT 'NONE',"
		      "  UNIQUE (user_id, station_id, network_id,"
		      "   network_type));"
		      "create index if not exists access_point_index"
		      " on access_point"
		      " (user_id, station_id, network_id, network_type);"

		      "create table if not exists access_point_scan"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  "SQL_TIME_COLS", network_type);"

		      /* When we saw an access point and some
			 attributes.  APSID is the OID of the access
			 point scan in the ACCESS_POINT_SCAN_LOG.
			 APID is the OID of the access point if the
			 ACCESS_POINT table.  */
		      "create table if not exists access_point_log"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  APSID, APID, flags, "
		      "  signal_strength_normalized, signal_strength_db);"

		      "create view if not exists access_point_scan_combined as"
		      " select * from"
		      "  access_point_scan, access_point, access_point_log"
		      "  where access_point_log.APSID = access_point_scan.OID"
		      "    and access_point_log.APID = access_point.OID;"

		      "create table if not exists cells"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  lac, cell_id, network, country, network_type,"
		      "  operator,"
		      "  UNIQUE (lac, cell_id, network, country, network_type,"
		      "    operator));"
		      "create index if not exists cells_index"
		      " on cells"
		      " (lac, cell_id, network, country, network_type,"
		      "  operator);"

		      "create table if not exists cell_info"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  "SQL_TIME_COLS", cell_id, connected,"
		      "  network_type, signal_strength_normalized,"
		      "  signal_strength_dbm);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  logger_uploader_table_register (db_filename, "connection_configuration",
				  false);
  logger_uploader_table_register (db_filename, "device_configuration", false);
  logger_uploader_table_register (db_filename, "connection_stats", true);

  logger_uploader_table_register (db_filename, "access_point", false);
  logger_uploader_table_register (db_filename, "access_point_scan", true);
  logger_uploader_table_register (db_filename, "access_point_log", true);

  /* Initialize the network monitor.  */
  nm = nc_network_monitor_new ();

  g_signal_connect (G_OBJECT (nm), "new-connection",
		    G_CALLBACK (nm_new_connection), NULL);
  g_signal_connect (G_OBJECT (nm), "disconnected",
		    G_CALLBACK (nm_disconnected), NULL);
  g_signal_connect (G_OBJECT (nm), "default-connection-changed",
		    G_CALLBACK (nm_default_connection_changed), NULL);
  g_signal_connect (G_OBJECT (nm), "scan-results",
		    G_CALLBACK (nm_scan_results), NULL);
  g_signal_connect (G_OBJECT (nm), "cell-info-changed",
		    G_CALLBACK (nm_cell_info_changed), NULL);

  g_timeout_add_seconds (5 * 60, nm_connections_stat_cb, nm);
}

static
void nm_quit (void)
{
  if (nm)
    nm_connections_dump (nm, "DISCONNECTED");
  nm_stop_logging = now ();
}

/* User activity monitor.  */
static void
uam_idle_active (WCUserActivityMonitor *m, gboolean idle, int64_t t,
		 gpointer user_data)
{
  debug (5, DEBUG_BOLD ("The user is %s.  Previous state: "TIME_FMT),
	 idle == WC_USER_IDLE ? "idle" : "active", TIME_PRINTF (t));

  sqlq_append_printf (sqlq, false,
		      "insert into user_activity"
		      " ("SQL_TIME_COLS", previous_state, duration, new_state)"
		      " values ("TM_FMT", '%s', %"PRId64", '%s');",
		      TM_PRINTF (now_tm ()),
		      idle == WC_USER_IDLE ? "active" : "idle",
		      t,
		      idle == WC_USER_IDLE ? "idle" : "active");

  nm_scan_queue ();
}

static void
uam_init (void)
{
  /* Initialize the user activity monitor.  */
  WCUserActivityMonitor *m = wc_user_activity_monitor_new ();

  char *errmsg = NULL;
  int err;
  err = sqlite3_exec (db,
		      "create table if not exists user_activity"
		      "(OID INTEGER PRIMARY KEY AUTOINCREMENT, "
		      " "SQL_TIME_COLS", previous_state, duration, new_state);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  logger_uploader_table_register (db_filename, "user_activity", true);

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
  debug (4, "Battery status: "
	 "charging: %d -> %d; discharging: %d -> %d; "
	 "mV: %d -> %d; mAh: %d -> %d; charger: %s -> %s",
	 old_is_charging, is_charging,
	 old_is_discharging, is_discharging,
	 old_mv, mv, old_mah, mah,
	 wc_battery_charger_to_string (old_charger),
	 wc_battery_charger_to_string (charger));

  sqlq_append_printf (sqlq, false,
		      "insert into battery_log"
		      " ("SQL_TIME_COLS", id,"
		      "  is_charging, charger, is_discharging, voltage, mah)"
		      " values ("TM_FMT","
		      "  (select id from batteries where device = '%q'),"
		      "  '%d', '%q', %d, %d, %d);",
		      TM_PRINTF (now_tm ()), wc_battery_id (b),
		      is_charging, wc_battery_charger_to_string (charger),
		      is_discharging, mv, mah);
}

static void
bm_init (void)
{
  /* Initialize the battery monitor.  */
  WCBatteryMonitor *m = wc_battery_monitor_new ();

  char *errmsg = NULL;
  int err;
  err = sqlite3_exec (db,
		      /* A list of batteries.  */
		      "create table if not exists batteries"
		      " (id INTEGER PRIMARY KEY,"
		      "  device, voltage_design, mah_design,"
		      "  UNIQUE (device));"
		      "create index if not exists batteries_device_index"
		      " on batteries (device);"

		      /* ID is the ID of the battery in the BATTERIES
			 table.  */
		      "create table if not exists battery_log"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  "SQL_TIME_COLS", id, is_charging, charger, "
		      "  is_discharging, voltage, mah);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  logger_uploader_table_register (db_filename, "batteries", true);
  logger_uploader_table_register (db_filename, "battery_log", true);

  GSList *batteries = wc_battery_monitor_list (m);
  while (batteries)
    {
      WCBattery *b = WC_BATTERY (batteries->data);

      sqlq_append_printf (sqlq, false,
			  "insert or ignore into batteries"
			  " (device, voltage_design, mah_design)"
			  " values ('%q', %d, %d);",
			  wc_battery_id (b),
			  wc_battery_mv_design (b),
			  wc_battery_mah_design (b));

      debug (4, "Initial battery status %s: "
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
service_start_stopped (const char *dbus_name, struct wc_process *process,
		       const char *status)
{
  sqlq_append_printf (sqlq, false,
		      "insert into service_log"
		      " ("SQL_TIME_COLS",pid,exe,arg0,arg1,dbus_name,status)"
		      " values ("TM_FMT", %d, '%q', '%q', '%q', '%q',"
		      "  '%s');",
		      TM_PRINTF (now_tm ()),
		      process->pid, process->exe, process->arg0, process->arg1,
		      dbus_name, status);
}

static void
service_started (WCServiceMonitor *m,
		 const char *dbus_name, struct wc_process *process,
		 gpointer user_data)
{
  service_start_stopped (dbus_name, process, "started");
}

static void
service_stopped (WCServiceMonitor *m,
		 const char *dbus_name, struct wc_process *process,
		 gpointer user_data)
{
  service_start_stopped (dbus_name, process, "stopped");
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

  bool dotfile = false;
  char *prefix = "/home/user/.";
  if (strncmp (prefix, src, strlen (prefix)) == 0)
    dotfile = true;

  debug (4, "%d(%d): %s;%s;%s: %s ("DEBUG_BOLD("%s")"%s%s%s, "BYTES_FMT")",
	 cb->top_levels_pid, cb->actor_pid,
	 cb->top_levels_exe, cb->top_levels_arg0, cb->top_levels_arg1,
	 wc_process_monitor_cb_str (cb->cb),
	 dotfile ? "" : src,
	 dotfile ? src : "", dest ? " -> " : "", dest ?: "",
	 BYTES_PRINTF (stat->st_size));

  GString *s = g_string_new ("");
  GSList *l = services;
  for (l = services; l; l = l->next)
    {
      g_string_append (s, (char *) l->data);
      if (l->next)
	g_string_append (s, ";");
    }

  sqlq_append_printf (sqlq, false,
		      "insert into file_access_log"
		      " ("SQL_TIME_COLS","
		      "  dbus_name, "
		      "  service_pid, service_exe,"
		      "  service_arg0, service_arg1,"
		      "  actor_pid, actor_exe,"
		      "  actor_arg0, actor_arg1,"
		      "  action, src, dest, size)"
		      " values ("TM_FMT",%Q,%d,%Q,%Q,%Q,"
		      "  %d,%Q,%Q,%Q,%Q,%Q,%Q,%"PRId64");",
		      TM_PRINTF (now_tm ()), s->str,
		      cb->top_levels_pid, cb->top_levels_exe,
		      cb->top_levels_arg0, cb->top_levels_arg1,
		      cb->actor_pid, cb->actor_exe,
		      cb->actor_arg0, cb->actor_arg1,
		      wc_process_monitor_cb_str (cb->cb),
		      src, dest, stat->st_size);

  g_string_free (s, true);
}

static void
sm_init (void)
{
  char *errmsg = NULL;
  int err = sqlite3_exec (db,
			  /* STATUS is either "started" or "stopped".  */
			  "create table if not exists service_log"
			  " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
			  "  "SQL_TIME_COLS", pid, exe, arg0, arg1, dbus_name,"
			  "  status);"

			  "create table if not exists file_access_log"
			  " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
			  "  "SQL_TIME_COLS","
			  "  dbus_name, "
			  "  service_pid, service_exe,"
			  "  service_arg0, service_arg1,"
			  "  actor_pid, actor_exe,"
			  "  actor_arg0, actor_arg1,"
			  "  action, src, dest, size);",
			  NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  logger_uploader_table_register (db_filename, "service_log", true);

  WCServiceMonitor *m = wc_service_monitor_new ();

  GSList *processes = wc_service_monitor_list (m);
  while ((processes))
    {
      struct wc_process *p = processes->data;
      processes = g_slist_delete_link (processes, processes);

      GSList *l;
      for (l = p->dbus_names; l; l = l->next)
	{
	  const char *dbus_name = l->data;
	  service_started (m, dbus_name, p, NULL);
	}
    }

  g_signal_connect (G_OBJECT (m), "service-started",
		    G_CALLBACK (service_started), NULL);
  g_signal_connect (G_OBJECT (m), "service-stopped",
		    G_CALLBACK (service_stopped), NULL);
  g_signal_connect (G_OBJECT (m), "service-fs-access",
		    G_CALLBACK (service_fs_access), NULL);
}

/* The system uptime, in seconds.  */
static int64_t
uptime (void)
{
  const char *filename = "/proc/uptime";

  char *contents = NULL;
  gsize length = 0;
  GError *error = NULL;
  if (! g_file_get_contents (filename, &contents, &length, &error))
    {
      debug (0, "Error reading %s: %s", filename, error->message);
      g_error_free (error);
      error = NULL;
      return -1;
    }

  int64_t t = -1;
  if (length != 0)
    {
      /* Ensure that the string is NUL terminated.  This won't change
	 our result as the file contains two floats and we are only
	 interested in the first one.  */
      contents[length - 1] = 0;
      sscanf (contents, "%"PRId64, &t);
      debug (0, "UPTIME: %s -> %"PRId64, contents, t);
    }

  g_free (contents);

  return t;
}

static void
shutdown_log (const char *description)
{
  struct tm tm = now_tm ();
  sqlq_append_printf (sqlq, false,
		      "insert into system ("SQL_TIME_COLS", status, uptime)"
		      " values ("TM_FMT", '%s', %"PRId64");",
		      TM_PRINTF (tm), description, uptime ());
}

static void
shutdown (WCShutdownMonitor *m, const char *description, gpointer user_data)
{
  /* Don't buffer anything.  Soon, we're going to be violently forced
     to quit.  */
  sqlq_flush_delay_set (sqlq, 0);

  static bool stopped;
  if (stopped)
    {
      debug (0, "shutdown signalled again.  This time: %s", description);
      return;
    }
  stopped = true;

  shutdown_log (description);
}

static void
sdm_init (void)
{
  WCShutdownMonitor *m = wc_shutdown_monitor_new ();

  char *errmsg = NULL;
  int err = sqlite3_exec (db,
			  /* STATUS is either "started," "stopped" or
			     "shutdown".  UPTIME is the system's
			     uptime (in seconds).  */
			  "create table if not exists system"
			  " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
			  "  "SQL_TIME_COLS", status, uptime);",
			  NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  logger_uploader_table_register (db_filename, "system", true);

  shutdown_log ("started");

  g_signal_connect (G_OBJECT (m), "shutdown",
		    G_CALLBACK (shutdown), NULL);
}

static GMainLoop *loop;

static void
unix_signal_handler (WCSignalHandler *sh, struct signalfd_siginfo *si,
		     gpointer user_data)
{
  debug (0, "Got signal %s.", strsignal (si->ssi_signo));
  fprintf (stderr, "Got signal %s.", strsignal (si->ssi_signo));

  if (si->ssi_signo == SIGTERM || si->ssi_signo == SIGINT
      || si->ssi_signo == SIGQUIT || si->ssi_signo == SIGHUP)
    {
      debug (0, "Caught %s, quitting.", strsignal (si->ssi_signo));

      nm_quit ();

      sqlq_flush_delay_set (sqlq, 0);

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
#if HAVE_MAEMO
  if (! getenv ("DBUS_SESSION_BUS_ADDRESS"))
    {
      bool good = false;

      const char *filename = "/tmp/session_bus_address.user";

      char *contents = NULL;
      gsize length = 0;
      GError *error = NULL;
      if (! g_file_get_contents (filename, &contents, &length, &error))
	{
	  debug (0, "Error reading %s: %s", filename, error->message);
	  g_error_free (error);
	  error = NULL;
	}
      else
	{
	  const char *prefix = "export DBUS_SESSION_BUS_ADDRESS='";
	  if (length > strlen (prefix)
	      && memcmp (contents, prefix, strlen (prefix)) == 0)
	    {
	      char *s = contents + strlen (prefix);
	      char *end = strchr (s, '\'');
	      if (end)
		{
		  *end = 0;
		  debug (0, "Setting DBUS_SESSION_BUS_ADDRESS to %s", s);
		  good = true;
		  setenv ("DBUS_SESSION_BUS_ADDRESS", s, 1);
		}
	    }

	  g_free (contents);
	}

      if (! good)
	debug (0, "DBUS_SESSION_BUS_ADDRESS unset.  May crash soon.");
    }
#endif

  g_type_init ();
  g_thread_init (NULL);

  debug (0, DEBUG_BOLD ("STARTING"));
  debug (0, "smart-storage-logger compiled on %s %s", __DATE__, __TIME__);

  /* Check if the pid file is locked before forking.  If it is locked
     bail.  Otherwise, fork and then acquire it definitively.  */
  char *pidfilename = files_logfile ("pid");
  const char *ssl = "smart-storage-logger";
  pid_t owner = pidfile_check (pidfilename, ssl);
  if (owner)
    error (1, 0, "%s already running (pid: %d)", ssl, owner);


  char *log = files_logfile ("output");
  {
    gchar *contents = NULL;
    gsize length = 0;
    if (g_file_get_contents (log, &contents, &length, NULL))
      {
	debug (0, "Last instance's output: %s (%d bytes)",
	       contents, (int) length);
	g_free (contents);
      }
  }

  debug (0, "Daemonizing.  Further output will be sent to %s", log);

  /* See if we should fork.  */
  bool do_fork = true;
  {
    int i;
    for (i = 0; i < argc; i ++)
      if (strcmp (argv[i], "--no-fork") == 0)
	do_fork = false;
  }
  if (do_fork)
    {
      int err = daemon (0, 0);
      if (err)
	error (0, err, "Failed to daemonize");
    }

  /* Redirect stdout and stderr to the log file.  */
  {
    int log_fd = open (log, O_WRONLY | O_CREAT, 0660);
    dup2 (log_fd, STDOUT_FILENO);
    dup2 (log_fd, STDERR_FILENO);
    if (! (log_fd == STDOUT_FILENO || log_fd == STDERR_FILENO))
      close (log_fd);
  }
  free (log);

  /* Acquire the lock file.  */
  if ((owner = pidfile_acquire (pidfilename, ssl)))
    error (1, 0, "%s already running (pid: %d)", ssl, owner);
  free (pidfilename);


  /* Register the debug table for upload.  */
  const char *debug_output = debug_init_ ();
  logger_uploader_table_register (debug_output, "log", true);


  /* Open the logging DB.  */
  db_filename = files_logfile ("ssl.db");
  int err = sqlite3_open (db_filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   db_filename, sqlite3_errmsg (db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  /* Set up an sql queue.  Buffer data at most 20 seconds.  */
  sqlq = sqlq_new_static (db, sqlq_buffer, sizeof (sqlq_buffer), 20);

  /* Initialize the unix signal catcher.  */
  signal_handler_init ();

  /* Initialize each monitor.  */
  sdm_init ();
  nm_init ();
  uam_init ();
  bm_init ();
  sm_init ();

  logger_uploader_init ();

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  sqlq_flush (sqlq);

  return 0;
}
