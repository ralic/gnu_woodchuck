/* network-monitor.h - Network monitor interface.
   Copyright 2010 Neal H. Walfield <neal@walfield.org>

   This file is part of Netczar.

   Netczar is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   Netczar is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.  */

#ifndef NETCZAR_NETWORK_MONITOR_H
#define NETCZAR_NETWORK_MONITOR_H

#include <glib-object.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "config.h"

/* There are two main object types: the network monitor and the
   network connection.  The network monitor is a singleton and its
   primarily purpose is to provide a handle for network connected and
   disconnected signals.  A network connection object is created when
   a new network connection is established.

   After creating a network monitor, any existing connections are
   queried after the main loop services any idle callbacks.  This
   provides the caller an opportunity to attach to the "connected" and
   "disconnected" signals.  */

/* Network Monitor's interface.  */
typedef struct _NCNetworkMonitor NCNetworkMonitor;
typedef struct _NCNetworkMonitorClass NCNetworkMonitorClass;

#define NC_NETWORK_MONITOR_TYPE (nc_network_monitor_get_type ())
#define NC_NETWORK_MONITOR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), NC_NETWORK_MONITOR_TYPE, NCNetworkMonitor))
#define NC_NETWORK_MONITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), NC_NETWORK_MONITOR_TYPE, NCNetworkMonitorClass))
#define IS_NC_NETWORK_MONITOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NC_NETWORK_MONITOR_TYPE))
#define IS_NC_NETWORK_MONITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NC_NETWORK_MONITOR_TYPE))
#define NC_NETWORK_MONITOR_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), NC_NETWORK_MONITOR_TYPE, NCNetworkMonitorClass))

struct _NCNetworkMonitorClass
{
  GObjectClass parent;

  /* "new-connection" signal: A new connection was established.
     Passed a (new) NC_NETWORK_CONNECTION object.  */
  guint new_connection_signal_id;

  /* "disconnected" signal: A new connection was established.  Passed
     an NC_NETWORK_CONNECTION object.

     This would perhaps be more appropriately emitted on an
     NC_NETWORK_CONNECTION object, but then we would have to set up
     signals, etc., which is just extra work and overhead.

     When this signal is received, no information about the underlying
     devices is available.  (However, the number of bytes transferred
     is available.)  */
  guint disconnected_signal_id;

  /* "default-connection-changed" signal: The default connection
     changed.  Passed two NC_NETWORK_CONNECTION objects: the previous
     default connection, as of the last default-connection-changed
     signal (if any), and the new default connection. */
  guint default_connection_changed_signal_id;
};

extern GType nc_network_monitor_get_type (void);

/* Network Connection's interface.  */
typedef struct _NCNetworkConnection NCNetworkConnection;
typedef struct _NCNetworkConnectionClass NCNetworkConnectionClass;

#define NC_NETWORK_CONNECTION_TYPE (nc_network_connection_get_type ())
#define NC_NETWORK_CONNECTION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), NC_NETWORK_CONNECTION_TYPE, NCNetworkConnection))
#define NC_NETWORK_CONNECTION_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), NC_NETWORK_CONNECTION_TYPE, NCNetworkConnectionClass))
#define IS_NC_NETWORK_CONNECTION(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NC_NETWORK_CONNECTION_TYPE))
#define IS_NC_NETWORK_CONNECTION_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NC_NETWORK_CONNECTION_TYPE))
#define NC_NETWORK_CONNECTION_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), NC_NETWORK_CONNECTION_TYPE, NCNetworkConnectionClass))

struct _NCNetworkConnectionClass
{
  GObjectClass parent;
};

extern GType nc_network_connection_get_type (void);

/* Instantiate a network monitor.  After instantiating, immediately
   connect to the object's "connected" and "disconnected" signals:
   existing connections will be created at the next "idle" point.  */
extern NCNetworkMonitor *nc_network_monitor_new (void);

/* Returns a list of the currently active connections.  The caller
   must free the list.  Use g_list_free (list); No reference is
   returned to the connections.  */
extern GList *nc_network_monitor_connections (NCNetworkMonitor *m);

/* Return the default connection (if any).  No reference is obtained
   to the connection.  */
extern NCNetworkConnection *
  nc_network_monitor_default_connection (NCNetworkMonitor *m);

/* The type of medium.  The values were choosen so that a bitmask can
   be made.  */
enum nc_connection_medium
  {
    NC_CONNECTION_MEDIUM_UNKNOWN = 1 << 0,
    NC_CONNECTION_MEDIUM_ETHERNET = 1 << 1,
    NC_CONNECTION_MEDIUM_WIFI = 1 << 2,
    NC_CONNECTION_MEDIUM_CELLULAR = 1 << 3,
    NC_CONNECTION_MEDIUM_BLUETOOTH = 1 << 4,
  };

/* A human readable string corresponding to the mediums.  Must be
   freed using g_free.  */
static inline __attribute__ ((malloc)) char *
nc_connection_medium_to_string (uint32_t mediums)
{
  if (! mediums)
    return NULL;

  char *s = g_strdup_printf
    ("%s%s%s%s%s",
     (NC_CONNECTION_MEDIUM_UNKNOWN & mediums) ? "unknown " : "",
     (NC_CONNECTION_MEDIUM_ETHERNET & mediums) ? "ethernet " : "",
     (NC_CONNECTION_MEDIUM_WIFI & mediums) ? "wifi " : "",
     (NC_CONNECTION_MEDIUM_CELLULAR & mediums) ? "cellular " : "",
     (NC_CONNECTION_MEDIUM_BLUETOOTH & mediums) ? "bluetooth " : "");

  /* Return the trailing space.  */
  s[strlen (s) - 1] = 0;

  return s;
}

/* Return a mask of the mediums this connection uses.  */
extern uint32_t nc_network_connection_mediums (NCNetworkConnection *nc);

enum
  {
    NC_DEVICE_INFO_IP_IP4_ADDR = 1 << 0,
    NC_DEVICE_INFO_IP_IP6_ADDR = 1 << 1,
    NC_DEVICE_INFO_IP_ADDR
      = NC_DEVICE_INFO_IP_IP4_ADDR | NC_DEVICE_INFO_IP_IP6_ADDR,

    NC_DEVICE_INFO_GATEWAY_IP4_ADDR = 1 << 2,
    NC_DEVICE_INFO_GATEWAY_IP6_ADDR = 1 << 3,
    NC_DEVICE_INFO_GATEWAY_IP_ADDR
      = NC_DEVICE_INFO_GATEWAY_IP4_ADDR | NC_DEVICE_INFO_GATEWAY_IP6_ADDR,

    NC_DEVICE_INFO_GATEWAY_MAC_ADDR = 1 << 4,

    /* e.g., eth0.  */
    NC_DEVICE_INFO_INTERFACE = 1 << 5,
    /* In the case of WiFi, the access point's name.  */
    NC_DEVICE_INFO_ACCESS_POINT = 1 << 6,

    NC_DEVICE_INFO_STATS = 1 << 7,

    NC_DEVICE_INFO_MEDIUM = 1 << 8,

    NC_DEVICE_INFO_ALL = -1
  };

struct nc_stats
{
  /* Number of bytes sent/received.  */
  uint64_t tx;
  uint64_t rx;
  /* The time the statistics were collected (milliseconds since the
     epoch).  */
  uint64_t time;
};

struct nc_device_info
{
  /* Which fields are valid.  */
  uint32_t mask;

  union
  {
    /* Valid if NC_DEVICE_INFO_IP_IP4_ADDR is set.  */
    uint8_t ip4[4];
    /* Valid if NC_DEVICE_INFO_IP_IP6_ADDR is set.  */
    uint8_t ip6[16];
  };
  union
  {
    /* Valid if NC_DEVICE_INFO_GATEWAY_IP4_ADDR is set.  */
    uint8_t gateway4[4];
    /* Valid if NC_DEVICE_INFO_GATEWAY_IP6_ADDR is set.  */
    uint8_t gateway6[16];
  };
  /* Valid if NC_DEVICE_INFO_GATEWAY_MAC_ADDR is set.  */
  uint8_t gateway_hwaddr[6];

  char *access_point;

  /* Valid if NC_DEVICE_INFO_INTERFACE is set.  */
  char *interface;

  /* Valid if NC_DEVICE_INFO_MEDIUM is set.  */
  enum nc_connection_medium medium;

  struct nc_stats stats;
};

/* Returns a list of struct nc_device_info for each device used by the
   connection.  Only those elements which are included in MASK are
   guaranteed to be queried.  The caller must free the list and the
   elements.  Use:

     g_list_foreach (list, g_free, NULL);
     g_list_free (list);  */
GList *nc_network_connection_info (NCNetworkConnection *conn,
				   uint32_t mask);



/* Whether this connection is the default connection.  */
extern bool nc_network_connection_is_default (NCNetworkConnection *nc);

/* Returns the time at which the network connection was established
   (in MS past the epoch).  */
extern uint64_t nc_network_connection_time_established
  (NCNetworkConnection *nc);

/* A stable identifier for the life of the connection.  Connection
   names may be reused, however, it is guaranteed that no concurrent
   connections will have the same identifier.  */
extern const char *nc_network_connection_id (NCNetworkConnection *nc);

#endif
