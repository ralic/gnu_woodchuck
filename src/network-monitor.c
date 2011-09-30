/* network-monitor.c - Network monitor.
   Copyright 2010, 2011 Neal H. Walfield <neal@walfield.org>

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

#include "config.h"
#include "network-monitor.h"

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

#if HAVE_NETWORK_MANAGER
# include <NetworkManager/NetworkManager.h>
# include "org.freedesktop.NetworkManager.h"
# include "org.freedesktop.NetworkManager.Device.h"
# include "org.freedesktop.NetworkManager.Device.Gsm.h"
# include "org.freedesktop.NetworkManager.Device.Serial.h"
# include "org.freedesktop.NetworkManager.Device.Wired.h"
# include "org.freedesktop.NetworkManager.Device.Wireless.h"
# include "org.freedesktop.NetworkManager.AccessPoint.h"
#endif
#if HAVE_ICD2
# include <icd/dbus_api.h>
# include "com.nokia.icd2.h"
#endif
#include "marshal.h"

#include "dbus-util.h"
#include "util.h"
#include "debug.h"
#include "ll-networking-linux.h"

/* Network Infrastructure Model
   ----------------------------

   Internally, the network infrastructure consists of three types of
   objects: the system, network devices and network connections.  Only
   two of these are directly exposed to users of this subsystem: the
   network monitor and network connections.

   The system is represented by the network monitor.  There is exactly
   one instance of the network monitor.  This object is needed to
   detect new devices and to let the user know about new connections
   (some object is need to get gobject signals).

   A network device represents network devices: ethernet cards, WiFi
   cards, etc.  Network devices may be added or removed while the
   system is running (e.g., a usb network device).  Network devices
   are used to detect when a connection is established and to get
   properties about the underlying connection.  We keep information
   about all connected devices, whether a connection is associated
   with them or not.

   A network connection encapsulates a network connection.

   There is a many-to-many relationship between devices and
   connections: a network connection may use multiple devices, and a
   device may host multiple connections.


   ICD2 Notes
   ----------

   ICD2 does not directly expose devices.  Instead, we need to infer
   them from connection information.

   To track connection and device state, we listen for state_sig
   signals, which indicate the connection whose state *may* have
   changed and the new state.  On such a signal, we invoke the
   addrinfo_req method, which causes ICD2 to send addrinfo_sig
   signals, which indicate each connection's address(es).  Using the
   addresses, we are able to discover the underlying devices.

   On program start up, we subscribe to state_sig signals and send an
   addrinfo_req signal.  The latter allows us to determine the initial
   state of the system; the former to track state changes.

   The addrinfo signal does not indicate the state of a connection and
   the state signal does not indicate its addresses.  We need both to
   create a connection.  To work around this, we create the connection
   in state_sig without any devices and treat a connection in this
   state specially.  In particular, state changes are not published.
   For such connections, we request an addrinfo signal.  In addrinfo,
   we detect new connections due to their lack of devices.


   Front-end vs. Back-end
   ----------------------

   The front-end implementation (i.e., this file) is not free of
   back-end specific ifdefs.  However, much backend specific code has
   been split off to separate files.  Completing this split would have
   decreased the readability of the code, we think.  In places where
   it makes sense, functions are used that the backend implementation
   must define.

   To avoid exposing functions and types, we #include the backend code
   at the end of this file.  This enables us to use static everywhere.
*/

typedef struct _NCNetworkDevice NCNetworkDevice;
typedef struct _NCNetworkDeviceClass NCNetworkDeviceClass;

#define NC_NETWORK_DEVICE_TYPE (nc_network_device_get_type ())
#define NC_NETWORK_DEVICE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), NC_NETWORK_DEVICE_TYPE, NCNetworkDevice))
#define NC_NETWORK_DEVICE_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), NC_NETWORK_DEVICE_TYPE, NCNetworkDeviceClass))
#define IS_NC_NETWORK_DEVICE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NC_NETWORK_DEVICE_TYPE))
#define IS_NC_NETWORK_DEVICE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NC_NETWORK_DEVICE_TYPE))
#define NC_NETWORK_DEVICE_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), NC_NETWORK_DEVICE_TYPE, NCNetworkDeviceClass))

/* Per connection device information.  */
struct per_connection_device_state
{
  /* The stats of the device at the time of the connect.  */
  struct nc_stats stats_connect;

  /* A cached pointer to the device.  We resolve the reference lazily
     as it may be that a connection is created before all the devices
     it uses have been locally instantiated.  */
  NCNetworkDevice *device;
  /* The device's name (which must match the corresponding device's
     NCNetworkDevice->name field).  */
  char device_name[];
};

struct _NCNetworkConnection
{
  GObject parent;

  /* For NM, the dbus object.  For icd2, the connection's 'network
     id'  */
  char *name;

  /* Back pointer to the network monitor.  */
  NCNetworkMonitor *network_monitor;

  /* Per-connection device data (struct per_connection_device_state).  */
  GList *per_connection_device_state;

#if HAVE_NETWORK_MANAGER
  /* Object: self->name
     Interface: "org.freedesktop.NetworkManager.Connection.Active" */
  DBusGProxy *connection_active_proxy;
#endif

  /* Set using connection_state_set.  */
  int state;

  /* The time (in ms since the epoch) at which the connection was
     established / disconnected.  */
  uint64_t connected_at;
  uint64_t disconnected_at;

#if HAVE_NETWORK_MANAGER
  /* Used by active_connections_scan_cb.  */
  bool seen;
#endif
};

struct _NCNetworkDeviceClass
{
  GObjectClass parent;
};

static GType nc_network_device_get_type (void);

static NCNetworkDevice *nc_network_device_new
  (NCNetworkMonitor *network_monitor, const char *name_device,
   const char *interface, int medium);

struct _NCNetworkDevice
{
  GObject parent;

  /* In the case of NM, the dbus object.  For ICD2, the device's OS
     interface, e.g., wlan0).  */
  char *name;

  /* Back pointer to the network monitor.  */
  NCNetworkMonitor *network_monitor;

#if HAVE_NETWORK_MANAGER
  /* Proxy object for the device.  */
  DBusGProxy *device_proxy;
#endif

  int medium;
  int state;

  char *interface;

#if HAVE_NETWORK_MANAGER
  /* Valid if self->MEDIUM == NC_CONNECTION_MEDIUM_WIFI.  */
  /* Object: self->name
     Interface: "org.freedesktop.NetworkManager.Connection.Active" */
  char *access_point_dbus_object;
  DBusGProxy *access_point_proxy;
#endif
  char *ssid;

#if HAVE_NETWORK_MANAGER
  char *ip4_config_dbus_object;
  DBusGProxy *ip4_config_proxy;
#endif

  struct nc_stats stats;
  uint64_t stats_fetched_at;
};

struct _NCNetworkMonitor
{
  GObject parent;

  /* System bus.  */
  DBusGConnection *system_bus;

#if HAVE_NETWORK_MANAGER
  /* Proxy object for the network manager.
     Object: /org/freedesktop/NetworkManager
     Interface: org.freedesktop.NetworkManager  */
  DBusGProxy *network_manager_proxy;
#endif
#if HAVE_ICD2
  /* Proxy object for the icd2 daemon.
     Object: ICD_DBUS_API_PATH
     Interface: ICD_DBUS_API_INTERFACE  */
  DBusGProxy *icd2_proxy;
  DBusGProxy *icd2_proxy2;
  /* Proxy object for the phone net service.
     Object: /com/nokia/phone/net
     Interface: "Phone.Net"  */
  DBusGProxy *phone_net_proxy;
  /* Proxy object for the GRPS services.
     Object: /com/nokia/csd/GPRS
     Interface: "com.nokia.csd.GPRS"  */
  DBusGProxy *gprs_proxy;
#endif

  /* List of currently attached devices (NCNetworkDevice).  Recall: a
     device may, but need not, be associated with an active network
     connection.  */
  GList *devices;
  /* List of active connections (NCNetworkConnection).  */
  GList *connections;


  /* The idle loop id of the active connections scan.  */
  guint active_connections_scan_pending_id;


  /* default_connection is the known default connection.  To avoid a
     number of default-connection-changed events in quick succession,
     when we detect a change, we first save it in
     default_connection_real and set an idle callback
     (default_connection_signal_source).  That callback sends any required
     signal and updates default_connection appropriately.  */
  NCNetworkConnection *default_connection;
  NCNetworkConnection *default_connection_real;
  guint default_connection_signal_source;
#if HAVE_ICD2
  guint default_connection_scan_source;
#endif

  /* The last time interface statistics were collected.  */
  uint64_t stats_last_updated_at;

#if HAVE_ICD2
  guint addrinfo_req_source;
  guint state_req_source;
#endif

#if HAVE_ICD2
  /* A hash from NETWORK_TYPE to GSList's.  The first element of the
     GSList is a string containing the key.  The remaining elements
     are struct nm_aps.  */
  GHashTable *network_type_to_scan_results_hash;
  int am_scanning;
  uint64_t scan_completed;
#endif

#if HAVE_ICD2
  /* Cell info for the currently connected tower.  */
  struct nm_cell cell_info;
#endif

#if HAVE_ICD2
  /* After ICD2 indicates that a direct GPRS connection (i.e.,
     non-tethered connection) goes down, we still get some empty
     gprs.Status signals.  This suggests We don't want to build up a
     new tethered connection.  To prevent this, we ignore such
     messages if they occur shortly after a detached message.  */
  uint64_t gprs_direct_connection;
#endif
};

static void nc_network_monitor_state_dump (NCNetworkMonitor *m);

static const char *device_state_to_str (int state);
static const char *connection_state_to_str (int state);

/* The following states should only be used to force a device or
   connection into a specific state, e.g., when bringing down the
   device.  Otherwise, to determine if a connection or device is
   connected, the following functions should be used.  */
#if HAVE_NETWORK_MANAGER
# define DEVICE_STATE_DISCONNECTED NM_DEVICE_STATE_DISCONNECTED
# define DEVICE_STATE_CONNECTED NM_DEVICE_STATE_ACTIVATED
# define CONNECTION_STATE_DISCONNECTED NM_ACTIVE_CONNECTION_STATE_ACTIVATING
# define CONNECTION_STATE_CONNECTED NM_ACTIVE_CONNECTION_STATE_ACTIVATED
#endif
#if HAVE_ICD2
# define DEVICE_STATE_DISCONNECTED ICD_STATE_DISCONNECTED
# define DEVICE_STATE_CONNECTED ICD_STATE_CONNECTED
# define CONNECTION_STATE_DISCONNECTED ICD_STATE_DISCONNECTED
# define CONNECTION_STATE_CONNECTED ICD_STATE_CONNECTED
#endif

/* Return whether STATE is a connected state or not.  */
static bool connection_state_is_connected (int state);
static bool device_state_is_connected (int state);

/* Look up a device by its NCNetworkDevice->name (NM: the device's
   dbus object path; ICD2: the interface name, e.g., wlan0).  If
   known, returns the corresponding NCNetworkDevice object.  Note that
   this does NOT add a reference.  */
static NCNetworkDevice *
device_name_to_device (NCNetworkMonitor *network_monitor,
		       const char *device)
{
  GList *e;
  for (e = network_monitor->devices; e; e = e->next)
    {
      NCNetworkDevice *d = NC_NETWORK_DEVICE (e->data);
      if (strcmp (d->name, device) == 0)
	return d;
    }

  GString *s = g_string_new ("");
  g_string_append_printf (s, "Device %s unknown. %d known devices:",
			  device, g_list_length (network_monitor->devices));
  for (e = network_monitor->devices; e; e = e->next)
    {
      NCNetworkDevice *d = NC_NETWORK_DEVICE (e->data);
      g_string_append_printf (s, " %s", d->name);
    }
  debug (0, "%s", s->str);
  g_string_free (s, true);

  return NULL;
}

/* Look up a device by its interface name.  If known, returns the
   corresponding NCNetworkDevice object.  Note that this does NOT add
   a reference.  */
static NCNetworkDevice *
device_interface_to_device (NCNetworkMonitor *network_monitor,
			    const char *interface)
{
  GList *e;
  for (e = network_monitor->devices; e; e = e->next)
    {
      NCNetworkDevice *d = NC_NETWORK_DEVICE (e->data);
      if (strcmp (d->interface, interface) == 0)
	return d;
    }

  /* This warning is pretty annoying as there are usually a number of
     interfaces that network manager does not manage (or have devices
     for).  */
  debug (5, "No device uses interface %s.", interface);
  return NULL;
}

/* Look up a connection by its NCNetworkConnection->name (NM: the
   device's dbus object path; ICD2: the service id).  If known,
   returns the corresponding NCNetworkConnection object.  Note that
   this does NOT add a reference.  */
static NCNetworkConnection *
connection_name_to_connection (NCNetworkMonitor *network_monitor,
			       const char *name)
{
  GList *e;
  for (e = network_monitor->connections; e; e = e->next)
    {
      NCNetworkConnection *c = NC_NETWORK_CONNECTION (e->data);
      if (strcmp (c->name, name) == 0)
	return c;
    }

  /* This warning is pretty annoying as there are usually a number of
     interfaces that network manager does not manage (or have devices
     for).  */
  debug (5, "No connection with name %s.", name);
  return NULL;
}

static void stats_update (NCNetworkMonitor *network_monitor, bool force);

static NCNetworkDevice *
per_connection_device_to_device (NCNetworkConnection *c,
				 struct per_connection_device_state *cd)
{
  if (! cd->device)
    /* Lazily resolve the reference.  */
    {
      cd->device = device_name_to_device (c->network_monitor,
					  cd->device_name);
      if (cd->device)
	{
	  /* Get the "initial" statistics.  Let's hope they are not
	     too out of date!  */
	  stats_update (c->network_monitor, true);
	  cd->stats_connect = cd->device->stats;
	}
    }

  return cd->device;
}

/* Update the statistics for all known devices.  */
static void
stats_update (NCNetworkMonitor *network_monitor, bool force)
{
  uint64_t n = now ();
  if (! force && n - network_monitor->stats_last_updated_at < 300)
    /* Less than 300ms since last update.  Don't do anything.  */
    return;

  network_monitor->stats_last_updated_at = n;

  bool cb (char *interface, char *stats)
  {
    NCNetworkDevice *d
      = device_interface_to_device (network_monitor, interface);
    if (! d)
      /* Device is not managed by us.  Ignore.  */
      return true;


    /* Kill the trailing newline.  */
    // Only needed if we need the default.
    // stats[l - 1] = 0;

    char *f[9];
    int count = split_line (stats, sizeof (f) / sizeof (*f), f);

    if (count >= 1)
      d->stats.rx = strtoll (f[0], NULL, 10);
    if (count >= 9)
      d->stats.tx = strtoll (f[8], NULL, 10);
    d->stats.time = n;

    debug (5, "Interface %s: %"PRId64"/%"PRId64,
	   interface, d->stats.rx, d->stats.tx);

    return true;
  }

  for_each_proc_net_dev (cb);
}

/* Default connection management.  When the default connection changes
   from one connection to another, we usually notice it in two steps:
   the old connection becomes not the default connection and the new
   connection becomes the default connection.  We really don't want to
   send two "default-connection-changed" signals.  To coalesce them,
   we use an idle handler.  We could instead use a timeout, but it
   seems an idle handler works well enough in practice.

   When the backend detects that a connection has become a default
   connection or is no longer the default connection, it should call
   default_connection_update.  This updates the default connection
   state internally and schedules the sending of the
   "default-connection-changed" signal.  */

/* Called by the front end when it detects interesting events, which
   suggest that the default connection may have changed.  */
static void default_connection_scan (NCNetworkMonitor *m);

static gboolean
default_connection_update_send_signal (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  debug (0, "Updating default: %s -> %s.",
	 m->default_connection ? m->default_connection->name : "none",
	 m->default_connection_real
	 ? m->default_connection_real->name : "none");

  if (m->default_connection == m->default_connection_real)
    /* The change was transient.   Nothing to do.  */
    ;
  else
    {
      NCNetworkConnection *old = m->default_connection;
      m->default_connection = m->default_connection_real;

      g_signal_emit (m,
		     NC_NETWORK_MONITOR_GET_CLASS (m)
		       ->default_connection_changed_signal_id,
		     0,
		     old, m->default_connection);
    }

  m->default_connection_signal_source = 0;

  /* Don't call again.  */
  return false;
}

/* Update the default connection.  If SET is true, C is the new
   default connection.  If SET is false, C was the default connection
   but is no longer.  */
static void
default_connection_update (NCNetworkMonitor *m,
			   NCNetworkConnection *c, bool set)
{
  /* Since we don't respect connect_state_is_connection (c->state), it
     is possible to have a disconnected connection be the default
     connection.  Weird, huh?  */
#warning XXX Respect connection_state_is_connection (c->state)?
  debug (4, "%s %s as default connection",
	 set ? "set" : "clear", c ? c->name : NULL);

  if (set)
    /* C is the new default connection.  */
    {
      if (c == m->default_connection_real)
	/* It already is the default connection.  */
	{
	  debug (5, "Setting default: %s is already default.",
		 c ? c->name : "none");
	  return;
	}

      debug (4, "Setting default: %s -> %s.",
	     m->default_connection_real
	     ? m->default_connection_real->name : "none",
	     c ? c->name : "none");

      m->default_connection_real = c;
    }
  else
    /* C is no longer the default connection.  */
    {
      if (c != m->default_connection_real)
	/* But, we don't actually thing it is the default connection.
	   Nothing to do.  */
	{
	  debug (5, "Clearing default: %s was not default, ignoring.",
		 c ? c->name : "none");
	  return;
	}
      
      debug (4, "Clearing default: %s.", c ? c->name : "none");

      m->default_connection_real = NULL;
    }

  /* Schedule the signal.  */
  if (! m->default_connection_signal_source)
    m->default_connection_signal_source
      = g_idle_add (default_connection_update_send_signal, m);
}

/* The implementation of the network connection object.  */

static void nc_network_connection_dispose (GObject *object);

G_DEFINE_TYPE (NCNetworkConnection, nc_network_connection, G_TYPE_OBJECT);

static void
nc_network_connection_class_init (NCNetworkConnectionClass *klass)
{
  nc_network_connection_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = nc_network_connection_dispose;

  // NCNetworkConnectionClass *nc_network_connection_class = NC_NETWORK_CONNECTION_CLASS (klass);
}

static void
nc_network_connection_init (NCNetworkConnection *c)
{
  c->state = CONNECTION_STATE_DISCONNECTED;
}

/* See comment for nc_network_connection_new.  */
static void
connection_state_set (NCNetworkConnection *c, int state,
		      bool initial_state)
{
  debug (0, "%s: %s -> %s", c->name,
	 initial_state ? "initial" : connection_state_to_str (c->state),
	 connection_state_to_str (state));

  NCNetworkMonitor *m = c->network_monitor;

  if (! initial_state && c->state == state)
    return;

  default_connection_scan (c->network_monitor);

  int ostate = c->state;
  c->state = state;

  if (! c->per_connection_device_state)
    /* This connection has not been published yet.  */
    {
      debug (0, "Not publishing state change for uninitialized connection %s",
	     c->name);
      return;
    }

  debug (0, "Connected? %s -> %s",
	 connection_state_is_connected (ostate) ? "true" : "false",
	 connection_state_is_connected (state) ? "true" : "false");

  if ((initial_state || ! connection_state_is_connected (ostate))
      && connection_state_is_connected (state))
    /* Now connected.  */
    {
      c->connected_at = now ();
      stats_update (m, true);

      g_signal_emit (m,
		     NC_NETWORK_MONITOR_GET_CLASS
		       (m)->new_connection_signal_id, 0, c);
    }
  else if (connection_state_is_connected (ostate)
	   && ! connection_state_is_connected (state))
    /* Now disconnected.  (If this is the initial state, then there is
       nothing to tell the user about.)  */
    {
      g_signal_emit (m,
		     NC_NETWORK_MONITOR_GET_CLASS
		       (m)->disconnected_signal_id, 0, c);
      g_object_unref (c);
    }

  do_debug (5)
    nc_network_monitor_state_dump (m);
}

/* See comment for nc_network_connection_new.  */
static void
connection_add_device (NCNetworkConnection *c, const char *name)
{
  debug (0, "Adding device %s to connection %s", name, c->name);
  int l = strlen (name);

  struct per_connection_device_state *cd = g_malloc0 (sizeof (*cd) + l + 1);
  c->per_connection_device_state
    = g_list_append (c->per_connection_device_state, cd);

  memcpy (cd->device_name, name, l);

  /* Try to resolve CD->DEVICE now and fill in CD->STATS_CONNECT.  */
  per_connection_device_to_device (c, cd);
}

/* To create a new connection, first call nc_network_connection_new.
   Then add the associated devices using connection_add_device.
   Finally, call connection_state_set (with inital_state = true) to
   finish it up.  */
static void nc_network_connection_backend_new (NCNetworkConnection *c);

static NCNetworkConnection *
nc_network_connection_new (NCNetworkMonitor *network_monitor,
			   const char *name)
{
  NCNetworkConnection *c
    = NC_NETWORK_CONNECTION (g_object_new (NC_NETWORK_CONNECTION_TYPE, NULL));

  c->network_monitor = network_monitor;
  c->name = g_strdup (name);

  network_monitor->connections
    = g_list_prepend (network_monitor->connections, c);

  nc_network_connection_backend_new (c);

  return c;
}

static void
nc_network_connection_dispose (GObject *object)
{
  NCNetworkConnection *c = NC_NETWORK_CONNECTION (object);
  NCNetworkMonitor *m = c->network_monitor;

  debug (0, DEBUG_BOLD ("Disposing %s"), c->name);

  if (c->connected_at)
    {
      uint64_t n = now ();
      printf ("%s connected "TIME_FMT"\n",
	      c->name,
	      TIME_PRINTF (n - c->connected_at));
    }

  /* Definately not the default connection any more.  Do this first so
     that all methods still work.  */
  default_connection_update (m, c, false);
  if (m->default_connection_signal_source)
    default_connection_update_send_signal (m);
  
  m->connections = g_list_remove (m->connections, c);

  /* Free the per connection device state.  */
  GList *n = c->per_connection_device_state;
  c->per_connection_device_state = NULL;
  while (n)
    {
      GList *e = n;
      n = e->next;

      g_free (e->data);
      g_list_free_1 (e);
    }

#if HAVE_NETWORK_MANAGER
  if (c->connection_active_proxy)
    {
      g_object_unref (c->connection_active_proxy);
      c->connection_active_proxy = NULL;
    }
#endif

  g_free (c->name);
  c->name = NULL;

  do_debug (5)
    nc_network_monitor_state_dump (m);

  /* Chain up to the parent class */
  G_OBJECT_CLASS (nc_network_connection_parent_class)->dispose (object);
}

uint32_t
nc_network_connection_mediums (NCNetworkConnection *c)
{
  uint32_t mediums = 0;
  GList *e;
  for (e = c->per_connection_device_state; e; e = e->next)
    {
      struct per_connection_device_state *cd = e->data;
      NCNetworkDevice *d = per_connection_device_to_device (c, cd);

      mediums |= d->medium;
    }

  return mediums;
}

GList *
nc_network_connection_info (NCNetworkConnection *c, uint32_t mask)
{
  if ((mask & NC_DEVICE_INFO_STATS))
    /* We only need to do this once for all devices.  */
    stats_update (c->network_monitor, false);

  GList *ret = NULL;

  if (! c->per_connection_device_state)
    /* This connection has not been published yet.  */
    {
      debug (0, "Connection %s has no device state",
	     c->name);
      return NULL;
    }

  GList *e;
  for (e = c->per_connection_device_state; e; e = e->next)
    {
      struct per_connection_device_state *cd = e->data;

      NCNetworkDevice *d = per_connection_device_to_device (c, cd);
      if (! d)
	/* XXX: For some reason, a connection is associated with a
	   device which is not actually known to us. Here we hope that
	   this is a transient fault and that next time, every thing
	   will be good...  */
	{
	  debug (0, "Device %s unknown (associated with interface %s).",
		 cd->device_name, c->name);
	  continue;
	}

      struct nc_device_info *info = NULL;

      int extra = 0;
      if ((mask & NC_DEVICE_INFO_INTERFACE))
	{
	  if (d->interface)
	    {
	      int l = strlen (d->interface) + 1;
	      extra += l;
	    }
	}

      if ((mask & NC_DEVICE_INFO_ACCESS_POINT))
	{
	  if (d->ssid)
	    {
	      int l = strlen (d->ssid) + 1;
	      extra += l;
	    }
	}

      info = g_malloc0 (sizeof (*info) + extra);
      ret = g_list_append (ret, info);

      char *end = (void *) info + sizeof (*info);

      if ((mask & NC_DEVICE_INFO_INTERFACE) && d->interface)
	{
	  info->mask |= NC_DEVICE_INFO_INTERFACE;
	  info->interface = end;
	  strcpy (end, d->interface);
	  end += strlen (end) + 1;
	}
      if ((mask & NC_DEVICE_INFO_ACCESS_POINT) && d->ssid)
	{
	  info->mask |= NC_DEVICE_INFO_ACCESS_POINT;
	  info->access_point = end;
	  strcpy (end, d->ssid);
	  end += strlen (end) + 1;
	}

      assert ((uintptr_t) end - (uintptr_t) info == sizeof (*info) + extra);


#if HAVE_NETWORK_MANAGER
      if (mask & (NC_DEVICE_INFO_IP_IP4_ADDR
		  | NC_DEVICE_INFO_IP_IP6_ADDR
		  | NC_DEVICE_INFO_GATEWAY_IP4_ADDR
		  | NC_DEVICE_INFO_GATEWAY_IP6_ADDR
		  | NC_DEVICE_INFO_GATEWAY_MAC_ADDR))
	/* Getting the IP address also gets the gateway's IP and
	   vice-versa.  To get the gateway's MAC address, we need the
	   gateway's IP address.  */
	{
	  if (! d->ip4_config_proxy)
	    {
	      assert (! d->ip4_config_dbus_object);
	      d->ip4_config_dbus_object
		= dbus_property_lookup_str (d->device_proxy, NULL, NULL,
					    "Ip4Config");

	      debug (0, "%s's Ip4Config: %s",
		     d->name, d->ip4_config_dbus_object);

	      d->ip4_config_proxy = dbus_g_proxy_new_from_proxy
		(d->device_proxy,
		 "org.freedesktop.NetworkManager.IP4Config",
		 d->ip4_config_dbus_object);
	    }

	  if (d->ip4_config_dbus_object
	      && d->ip4_config_dbus_object[0] == '/'
	      && d->ip4_config_dbus_object[1])
	    /* An object path of "/" means that there is no ip4 object.  */
	    {
	      GValue value = { 0 };

	      static GType aau;
	      if (! aau)
		aau = dbus_g_type_get_collection ("GPtrArray",
						  DBUS_TYPE_G_UINT_ARRAY);
	      if (! dbus_property_lookup (d->ip4_config_proxy,
					  NULL, NULL, "Addresses",
					  aau, &value))
		debug (0, "Failed to lookup IP4 addresses.");
	      else
		{
		  GPtrArray *addresses = g_value_get_boxed (&value);
		  int i;
		  for (i = 0; addresses && i < addresses->len; i ++)
		    {
		      GArray *address = g_ptr_array_index (addresses, i);
		      if (address->len >= 1)
			{
			  uint32_t ip = g_array_index (address, guint32, 0);
			  info->ip4[0] = (ip >> 0) & 0xFF;
			  info->ip4[1] = (ip >> 8) & 0xFF;
			  info->ip4[2] = (ip >> 16) & 0xFF;
			  info->ip4[3] = (ip >> 24) & 0xFF;

			  info->mask |= NC_DEVICE_INFO_IP_IP4_ADDR;
			}
		      if (address->len >= 3)
			{
			  uint32_t ip = g_array_index (address, guint32, 2);
			  info->gateway4[0] = (ip >> 0) & 0xFF;
			  info->gateway4[1] = (ip >> 8) & 0xFF;
			  info->gateway4[2] = (ip >> 16) & 0xFF;
			  info->gateway4[3] = (ip >> 24) & 0xFF;

			  info->mask |= NC_DEVICE_INFO_GATEWAY_IP4_ADDR;
			}
		    }
		}
	    }
	}
#endif
#if HAVE_ICD2
      if ((mask & NC_DEVICE_INFO_IP_IP4_ADDR))
	{
	  in_addr_t ip = interface_to_ip (d->interface);
	  if (ip != INADDR_NONE)
	    {
	      info->ip4[0] = (ip >> 0) & 0xFF;
	      info->ip4[1] = (ip >> 8) & 0xFF;
	      info->ip4[2] = (ip >> 16) & 0xFF;
	      info->ip4[3] = (ip >> 24) & 0xFF;

	      info->mask |= NC_DEVICE_INFO_IP_IP4_ADDR;
	    }
	}

      if ((mask & (NC_DEVICE_INFO_GATEWAY_IP4_ADDR
		   | NC_DEVICE_INFO_GATEWAY_MAC_ADDR)))
	/* Get the gateway's IP address.  This also required to
	   get the gateway's MAC address.  */
	{
	  bool route_cb (char *interface, char *rest)
	  {
	    if (strcmp (interface, d->interface) != 0)
	      /* Interface does not match.  */
	      {
		debug (5, "Got interface '%s', want '%s'",
		       interface, d->interface);
		return true;
	      }

	    char *fields[3];
	    int count = split_line (rest,
				    sizeof (fields) / sizeof (fields[0]),
				    fields);
	    if (count != sizeof (fields) / sizeof (fields[0]))
	      {
		debug (0, "Misformed line! Got %d fields!", count);
		return true;
	      }

	    uint32_t dest = strtol (fields[0], NULL, 16);
	    if (! dest)
	      /* DEST is 0.0.0.0.  This is the default route for this
		 interface.  */
	      {
		uint32_t ip = strtol (fields[1], NULL, 16);
		debug (4, "gateway: %s via %s",
		       inet_ntoa ((struct in_addr) { ip }), d->interface);
		
		info->gateway4[0] = (ip >> 0) & 0xFF;
		info->gateway4[1] = (ip >> 8) & 0xFF;
		info->gateway4[2] = (ip >> 16) & 0xFF;
		info->gateway4[3] = (ip >> 24) & 0xFF;

		info->mask |= NC_DEVICE_INFO_GATEWAY_IP4_ADDR;

		/* We're done.  */
		return false;
	      }

	    /* Keep going.  */
	    return true;
	  }
	  for_each_proc_net_route (route_cb);
	}
#endif

      if ((mask & NC_DEVICE_INFO_GATEWAY_MAC_ADDR)
	  && (info->mask & NC_DEVICE_INFO_GATEWAY_IP4_ADDR))
	/* Get the gateway's MAC address.  This requires the
	   gateway's IP address.  */
	{
	  char gw[4 * 4];
	  sprintf (gw, "%d.%d.%d.%d",
		   info->gateway4[0], info->gateway4[1],
		   info->gateway4[2], info->gateway4[3]);

	  bool cb (char *ip, char *rest)
	  {
	    if (strcmp (gw, ip) != 0)
	      return true;

	    char *fields[3];
	    int count = split_line (rest,
				    sizeof (fields) / sizeof (fields[0]),
				    fields);
	    if (count != sizeof (fields) / sizeof (fields[0]))
	      {
		debug (0, "Misformed line! Got %d fields!", count);
		return true;
	      }

	    int a[6];
	    sscanf (fields[2], "%x:%x:%x:%x:%x:%x",
		    &a[0], &a[1], &a[2],
		    &a[3], &a[4], &a[5]);

	    int i;
	    for (i = 0; i < 6; i ++)
	      info->gateway_hwaddr[i] = a[i];

	    info->mask |= NC_DEVICE_INFO_GATEWAY_MAC_ADDR;

	    return false;
	  }
	  for_each_proc_net_arp (cb);
	}

      if ((mask & NC_DEVICE_INFO_STATS))
	{
	  info->mask |= NC_DEVICE_INFO_STATS;

	  info->stats.time = d->stats.time;
	  info->stats.tx = d->stats.tx - cd->stats_connect.tx;
	  info->stats.rx = d->stats.rx - cd->stats_connect.rx;
	}

      if ((mask & NC_DEVICE_INFO_MEDIUM))
	{
	  info->mask |= NC_DEVICE_INFO_MEDIUM;
	  info->medium = d->medium;
	}
    }

  return ret;
}

#if HAVE_ICD2
static gboolean default_connection_scan_cb (gpointer user_data);
#endif

bool
nc_network_connection_is_default (NCNetworkConnection *c)
{
#if HAVE_ICD2
  /* This won't deliver any pending signal.  That will still be done
     in the idle handler, however, it does ensure accurate
     information.  */
  if (c->network_monitor->default_connection_scan_source)
    {
      default_connection_scan_cb (c->network_monitor);
    }
#endif

  return c == c->network_monitor->default_connection;
}

uint64_t
nc_network_connection_time_established (NCNetworkConnection *c)
{
  return c->connected_at;
}

const char *
nc_network_connection_id (NCNetworkConnection *nc)
{
  return nc->name;
}

/* The implementation of the network device object.  */


static void nc_network_device_dispose (GObject *object);

G_DEFINE_TYPE (NCNetworkDevice, nc_network_device, G_TYPE_OBJECT);

static void
nc_network_device_class_init (NCNetworkDeviceClass *klass)
{
  nc_network_device_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = nc_network_device_dispose;

  // NCNetworkDeviceClass *nc_network_device_class = NC_NETWORK_DEVICE_CLASS (klass);
}

#if HAVE_NETWORK_MANAGER
static gboolean active_connections_scan_cb (gpointer user_data);
#endif

static void
device_state_changed (NCNetworkDevice *d, uint32_t new_state)
{
  if (new_state == d->state)
    /* Nothing to do.  */
    return;

  /* This function is not multi-threaded safe nor is it reentrant!  */
  static bool in;
  assert (! in);
  in = true;

  /* Whatever we do, first hang up any connection-dependent data.  */

#if HAVE_NETWORK_MANAGER
  if (d->access_point_proxy)
    {
      g_object_unref (d->access_point_proxy);
      d->access_point_proxy = NULL;

      g_free (d->access_point_dbus_object);
      d->access_point_dbus_object = NULL;
    }
#endif

  if (d->ssid)
    {
      g_free (d->ssid);
      d->ssid = NULL;
    }

#if HAVE_NETWORK_MANAGER
  if (d->ip4_config_dbus_object)
    {
      g_object_unref (d->ip4_config_proxy);
      d->ip4_config_proxy = NULL;

      g_free (d->ip4_config_dbus_object);
      d->ip4_config_dbus_object = NULL;
    }
#endif

  /* Transition to the new state.  */
  d->state = new_state;

  /* If the device has been activated and the device is a WiFi device,
     we likely are associated with an access point.  */
  if (device_state_is_connected (d->state)
      && d->medium == NC_CONNECTION_MEDIUM_WIFI)
    {
#if HAVE_NETWORK_MANAGER
      d->access_point_dbus_object = dbus_property_lookup_str
	(d->device_proxy, d->name,
	 "org.freedesktop.NetworkManager.Device.Wireless",
	 "ActiveAccessPoint");

      d->access_point_proxy = dbus_g_proxy_new_from_proxy
	(d->device_proxy,
	 "org.freedesktop.NetworkManager.AccessPoint",
	 d->access_point_dbus_object);

      d->ssid = dbus_property_lookup_str
	(d->access_point_proxy, NULL, NULL, "Ssid");
#endif
#if HAVE_ICD2
      d->ssid = interface_to_ssid (d->interface);
#endif
    }

#if HAVE_NETWORK_MANAGER
  /* The fact that a device changed state suggests that a connection
     also changed state.  Queue an active connection scan.  Note that
     we don't wnat to do the connection scan here as on startup (and
     perhaps at other times), we want to process all device changes
     before we scan for active connections.  */
  if (! d->network_monitor->active_connections_scan_pending_id)
    d->network_monitor->active_connections_scan_pending_id
      = g_idle_add (active_connections_scan_cb, d->network_monitor);
#endif

  /* Check if the default connection changed.  */
  default_connection_scan (d->network_monitor);

  assert (in);
  in = false;
}

static void
nc_network_device_init (NCNetworkDevice *d)
{
}

/* After calling this function, the backend should set up any backend
   specific details and then call device_state_changed to set the
   initial state.  */
static NCNetworkDevice *
nc_network_device_new (NCNetworkMonitor *network_monitor,
		       const char *name_device,
		       const char *interface, int medium)
{
  char *medium_str = nc_connection_medium_to_string (medium);
  debug (0, "New device: %s using %s, medium: %s",
	 name_device, interface, medium_str);
  g_free (medium_str);

  NCNetworkDevice *d
    = NC_NETWORK_DEVICE (g_object_new (NC_NETWORK_DEVICE_TYPE, NULL));

  d->network_monitor = network_monitor;
  network_monitor->devices = g_list_prepend (network_monitor->devices, d);

  d->name = g_strdup (name_device);
  d->medium = medium;
  d->state = DEVICE_STATE_DISCONNECTED;
  d->interface = g_strdup (interface);

  return d;
}

static void
nc_network_device_dispose (GObject *object)
{
  NCNetworkDevice *d = NC_NETWORK_DEVICE (object);

  debug (0, DEBUG_BOLD ("Disposing device %s"), d->name);

  g_free (d->interface);
  d->interface = NULL;

  if (d->network_monitor)
    {
      device_state_changed (d, DEVICE_STATE_DISCONNECTED);

      NCNetworkMonitor *m = d->network_monitor;

      GList *e = g_list_find (m->devices, d);
      assert (e);
      m->devices = g_list_delete_link (m->devices, e);

      d->network_monitor = NULL;
    }

  d->network_monitor->devices
    = g_list_remove (d->network_monitor->devices, d);

  g_free (d->name);
  d->name = NULL;

#if HAVE_NETWORK_MANAGER
  if (d->device_proxy)
    {
      g_object_unref (d->device_proxy);
      d->device_proxy = NULL;
    }
#endif

  /* Chain up to the parent class */
  G_OBJECT_CLASS (nc_network_device_parent_class)->dispose (object);
}

/* The implementation of the network monitor object.  */

/* The network service is a singleton.  */
static NCNetworkMonitor *nc_network_monitor;

static void nc_network_monitor_dispose (GObject *object);

G_DEFINE_TYPE (NCNetworkMonitor, nc_network_monitor, G_TYPE_OBJECT);

static void
nc_network_monitor_class_init (NCNetworkMonitorClass *klass)
{
  nc_network_monitor_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = nc_network_monitor_dispose;

  NCNetworkMonitorClass *nm_class = NC_NETWORK_MONITOR_CLASS (klass);
  nm_class->new_connection_signal_id
    = g_signal_new ("new-connection",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_marshal_VOID__POINTER,
		    G_TYPE_NONE, 1, G_TYPE_POINTER);

  nm_class->disconnected_signal_id
    = g_signal_new ("disconnected",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_marshal_VOID__POINTER,
		    G_TYPE_NONE, 1, G_TYPE_POINTER);

  nm_class->default_connection_changed_signal_id
    = g_signal_new ("default-connection-changed",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_user_marshal_VOID__POINTER_POINTER,
		    G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);

  nm_class->scan_results_signal_id
    = g_signal_new ("scan-results",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_user_marshal_VOID__POINTER,
		    G_TYPE_NONE, 1, G_TYPE_POINTER);

  nm_class->cell_info_changed_signal_id
    = g_signal_new ("cell-info-changed",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_user_marshal_VOID__POINTER,
		    G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void nc_network_monitor_backend_init (NCNetworkMonitor *m);

static void
nc_network_monitor_init (NCNetworkMonitor *m)
{
  GError *error = NULL;
  m->system_bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
  if (error)
    {
      g_critical ("Getting system bus: %s", error->message);
      g_error_free (error);
      return;
    }

  nc_network_monitor_backend_init (m);
}

NCNetworkMonitor *
nc_network_monitor_new (void)
{
  /* NC_NETWORK_MONITOR is a singleton.  */
  if (nc_network_monitor)
    g_object_ref (nc_network_monitor);
  else
    nc_network_monitor
      = NC_NETWORK_MONITOR (g_object_new (NC_NETWORK_MONITOR_TYPE, NULL));

  return nc_network_monitor;
}

static void
nc_network_monitor_dispose (GObject *object)
{
  g_critical ("Attempt to dispose network monitor singleton!");
}

GList *
nc_network_monitor_connections (NCNetworkMonitor *m)
{
  return g_list_copy (m->connections);
}

NCNetworkConnection *
nc_network_monitor_default_connection (NCNetworkMonitor *m)
{
  return m->default_connection;
}

static void
nc_network_monitor_state_dump (NCNetworkMonitor *m)
{
  debug (0, "Connections: %d; Devices: %d",
	 g_list_length (m->connections),
	 g_list_length (m->devices));
  GList *e;
  for (e = m->connections; e; e = e->next)
    {
      NCNetworkConnection *c = NC_NETWORK_CONNECTION (e->data);
      debug (0, "Connection %s (uses %d devices) %s",
	     c->name, g_list_length (c->per_connection_device_state),
	     connection_state_is_connected (c->state)
	     ? "connected" : "disconnected");

      GList *f;
      for (f = c->per_connection_device_state; f; f = f->next)
	{
	  struct per_connection_device_state *cd = f->data;
	  NCNetworkDevice *d = per_connection_device_to_device (c, cd);
	  debug (0, "  %s (%s)",
		 cd->device_name,
		 d ? d->interface : NULL);
	}
    }

  debug (0, "Known devices:");
  for (e = m->devices; e; e = e->next)
    {
      NCNetworkDevice *d = NC_NETWORK_DEVICE (e->data);
      debug (0, "  %s: %s", d->name, d->interface);
    }
}

/* Include the back-end specific details.  */
#if HAVE_ICD2
# include "network-monitor-icd2.c"
#endif
#if HAVE_NETWORK_MANAGER
# include "network-monitor-nm.c"
#endif
