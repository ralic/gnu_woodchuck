/* network.c - Network interface.
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

#include "network-monitor.h"
#include "config.h"

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
#endif

#include "org.freedesktop.NetworkManager.h"
#include "org.freedesktop.NetworkManager.Device.h"
#include "org.freedesktop.NetworkManager.Device.Gsm.h"
#include "org.freedesktop.NetworkManager.Device.Serial.h"
#include "org.freedesktop.NetworkManager.Device.Wired.h"
#include "org.freedesktop.NetworkManager.Device.Wireless.h"
#include "org.freedesktop.NetworkManager.AccessPoint.h"
#include "marshal.h"

#include "dbus-util.h"
#include "util.h"
#include "debug.h"

/* Network Infrastructure Model
   ----------------------------

   Internally, the network infrastructure consists of three types of
   objects: the system, network devices and network connections.  Only
   two of these are directly exposed to users of this subsystem: the
   network monitor and network connections.

   The system is represented by the network monitor.  There is exactly
   one instance of the network monitor.  This object is needed to
   detected new devices and to let the user know about new
   connections (some object is need to get gobject signals).

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

  /* A cached pointer to the device.  */
  NCNetworkDevice *device;
  /* The dbus object name of the device.  */
  char device_dbus_object[];
};

struct _NCNetworkConnection
{
  GObject parent;

  char *dbus_object;

  /* Back pointer to the network monitor.  */
  NCNetworkMonitor *network_monitor;

  /* Per-connection device data (struct per_connection_device_state).  */
  GList *devices;

  /* Object: self->dbus_object
     Interface: "org.freedesktop.NetworkManager.Connection.Active" */
  DBusGProxy *connection_active_proxy;

  /* The time (in ms since the epoch) at which the connection was
     established / disconnected.  */
  uint64_t connected_at;
  uint64_t disconnected_at;

  /* Used by nc_network_connection_new.  */
  bool seen;
};

struct _NCNetworkDeviceClass
{
  GObjectClass parent;
};

extern GType nc_network_device_get_type (void);

static NCNetworkDevice *nc_network_device_new
  (NCNetworkMonitor *network_monitor, const char *device);

struct _NCNetworkDevice
{
  GObject parent;

  char *dbus_object;

  /* Back pointer to the network monitor.  */
  NCNetworkMonitor *network_monitor;

  /* Proxy object for the device.  */
  DBusGProxy *device_proxy;
  int medium;
  int state;

  char *interface;

  /* Valid if self->MEDIUM == NC_CONNECTION_MEDIUM_WIFI.  */
  /* Object: self->dbus_object
     Interface: "org.freedesktop.NetworkManager.Connection.Active" */
  char *access_point_dbus_object;
  DBusGProxy *access_point_proxy;
  char *ssid;

  char *ip4_config_dbus_object;
  DBusGProxy *ip4_config_proxy;

  struct nc_stats stats;
  uint64_t stats_fetched_at;
};

struct _NCNetworkMonitor
{
  GObject parent;

  /* System bus.  */
  DBusGConnection *system_bus;

  /* Proxy object for the network manager.
     Object: /org/freedesktop/NetworkManager
     Interface: org.freedesktop.NetworkManager  */
  DBusGProxy *network_manager_proxy;


  /* List of currently attached devices (NCNetworkDevice).  Recall: a
     device may, but need not, be associated with an active network
     connection.  */
  GList *devices;
  /* List of active connections (NCNetworkConnection).  */
  GList *connections;


  /* The idle loop id of the active connections scan.  */
  guint active_connections_scan_pending_id;


  /* */
  NCNetworkConnection *default_connection;
  NCNetworkConnection *default_connection_real;
  guint default_connection_idle_cb;


  uint64_t stats_last_updated_at;
};

/* Look up a device by its dbus name.  If known, returns the
   corresponding NCNetworkDevice object.  Note that this does NOT add
   a reference.  */
static NCNetworkDevice *
device_dbus_to_device (NCNetworkMonitor *network_monitor,
		       const char *device)
{
  GList *e;
  for (e = network_monitor->devices; e; e = e->next)
    {
      NCNetworkDevice *d = NC_NETWORK_DEVICE (e->data);
      if (strcmp (d->dbus_object, device) == 0)
	return d;
    }

  debug (0, "Device %s unknown.", device);
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

static NCNetworkDevice *
per_connection_device_to_device (NCNetworkConnection *c,
				 struct per_connection_device_state *cd)
{
  if (! cd->device)
    {
      cd->device = device_dbus_to_device (c->network_monitor,
					  cd->device_dbus_object);
      if (cd->device)
	cd->stats_connect = cd->device->stats;
    }

  return cd->device;
}

static void
stats_update (NCNetworkMonitor *network_monitor)
{
  uint64_t n = now ();
  if (n - network_monitor->stats_last_updated_at < 300)
    /* Less than 300ms since last update.  Don't do anything.  */
    return;

  network_monitor->stats_last_updated_at = n;

  FILE *f = fopen ("/proc/net/dev", "r");
  if (! f)
    {
      debug (0, "Failed to open /proc/net/dev: %m");
      return;
    }

  char *line = NULL;
  size_t line_bytes = 0;

  int lines = 0;
  int l;
  while ((l = getline (&line, &line_bytes, f)) != -1)
    {
      if (++ lines <= 2)
	/* First 2 lines are header information.  */
	continue;

      char *stats = strchr (line, ':');
      if (! stats)
	/* Hmm... bad line.  */
	continue;

      char *interface = line;
      while (*interface == ' ')
	interface ++;

      /* Null terminate the interface.  */
      *stats = 0;
      stats ++;

      NCNetworkDevice *d
	= device_interface_to_device (network_monitor, interface);
      if (! d)
	/* Device is not managed by us.  Ignore.  */
	continue;


      /* Kill the trailing newline.  */
      // Only needed if we need the default.
      // stats[l - 1] = 0;

      char *e[9];
      int i;
      char *p;
      for (i = 0, p = stats;
	   *p && i < sizeof (e) / sizeof (*e);
	   i ++)
	{
	  /* Skip spaces.  */
	  while (*p && *p == ' ')
	    {
	      *p = 0;
	      p ++;
	    }

	  e[i] = p;

	  /* Skip text.  */
	  while (*p && *p != ' ')
	    p ++;
	}

      d->stats.rx = strtoll (e[0], NULL, 10);
      d->stats.tx = strtoll (e[8], NULL, 10);
      d->stats.time = n;

      debug (0, "Interface %s: %"PRId64"/%"PRId64,
	     interface, d->stats.rx, d->stats.tx);
    }

  free (line);
  fclose (f);
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
  c->connected_at = time (NULL);
}

static gboolean
default_connection_update_send_signal (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  debug (0, "Updating default: %s -> %s.",
	 m->default_connection ? m->default_connection->dbus_object : "none",
	 m->default_connection_real
	 ? m->default_connection_real->dbus_object : "none");

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

  m->default_connection_idle_cb = 0;

  /* Don't call again.  */
  return false;
}

/* Update the default connection.  If SET is true, C is the new
   default connection.  If SET is false, C was the default connection
   but is no longer.  */
static void
default_connection_update (NCNetworkConnection *c, bool set)
{
  if (set)
    /* C is the new default connection.  */
    {
      if (c == c->network_monitor->default_connection_real)
	/* It already is the default connection.  */
	{
	  debug (0, "Setting default: %s is already default.",
		 c ? c->dbus_object : "none");
	  return;
	}

      debug (0, "Setting default: %s -> %s.",
	     c->network_monitor->default_connection_real
	     ? c->network_monitor->default_connection_real->dbus_object
	     : "none",
	     c ? c->dbus_object : "none");

      c->network_monitor->default_connection_real = c;
    }
  else
    /* C is no longer the default connection.  */
    {
      if (c != c->network_monitor->default_connection_real)
	/* But, we don't actually thing it is the default connection.
	   Nothing to do.  */
	{
	  debug (0, "Clearing default: %s was not default, ignoring.",
		 c ? c->dbus_object : "none");
	  return;
	}
      
      debug (0, "Clearing default: %s.",
	     c ? c->dbus_object : "none");

      c->network_monitor->default_connection_real = NULL;
    }

  /* Schedule the signal.  */
  if (! c->network_monitor->default_connection_idle_cb)
    {
      c->network_monitor->default_connection_idle_cb
	= g_idle_add (default_connection_update_send_signal,
		      c->network_monitor);
    }
}

static void
connection_connection_active_properties_changed_cb (DBusGProxy *proxy,
						    GHashTable *properties,
						    NCNetworkConnection *c)
{
  assert (c->connection_active_proxy == proxy);

  void iter (gpointer key, gpointer data, gpointer user_data)
  {
    GValue *value = data;

    debug (0, "%s: key %s changed", c->dbus_object, key);

    if (strcmp (key, "Default") == 0)
      /* The default route changed!  */
      {
	if (G_VALUE_TYPE (value) != G_TYPE_BOOLEAN)
	  {
	    debug (0, "%s's type should be boolean but got %s",
		   key, g_type_name (G_VALUE_TYPE (value)));
	  }
	else
	  {
	    gboolean set = g_value_get_boolean (value);
	    default_connection_update (c, set);
	  }
      }
  }

  g_hash_table_foreach (properties, iter, NULL);
}

static NCNetworkConnection *
nc_network_connection_new (NCNetworkMonitor *network_monitor,
			   const char *dbus_object)
{
  NCNetworkConnection *c
    = NC_NETWORK_CONNECTION (g_object_new (NC_NETWORK_CONNECTION_TYPE, NULL));

  c->network_monitor = network_monitor;
  c->dbus_object = g_strdup (dbus_object);

  c->connection_active_proxy = dbus_g_proxy_new_from_proxy
    (network_monitor->network_manager_proxy,
     "org.freedesktop.NetworkManager.Connection.Active",
     c->dbus_object);

  static GType object_path_array_type;
  if (! object_path_array_type)
    object_path_array_type
      = dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH);

  GValue devices_value = { 0 };
  if (! dbus_property_lookup
      (c->connection_active_proxy, NULL, NULL, "Devices",
       object_path_array_type, &devices_value))
    {
      printf ("Failed to look up "
	      "org.freedesktop.NetworkManager.ActiveConnections.Devices\n");
    }
  else
    {
      printf ("Connection %s uses devices:\n", dbus_object);

      stats_update (network_monitor);

      GPtrArray *devices = g_value_get_boxed (&devices_value);
      int i;
      for (i = 0; i < devices->len; i ++)
	{
	  char *device_dbus_object = g_ptr_array_index (devices, i);
	  printf ("  %s\n", device_dbus_object);
	  int l = strlen (device_dbus_object);

	  struct per_connection_device_state *cd
	    = g_malloc0 (sizeof (*cd) + l + 1);
	  c->devices = g_list_append (c->devices, cd);

	  memcpy (cd->device_dbus_object, device_dbus_object, l);

	  per_connection_device_to_device (c, cd);
	}
    }

  dbus_g_proxy_add_signal (c->connection_active_proxy,
			   "PropertiesChanged",
			   DBUS_TYPE_G_MAP_OF_VARIANT,
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal
    (c->connection_active_proxy, "PropertiesChanged",
     G_CALLBACK (connection_connection_active_properties_changed_cb),
     c, NULL);

  network_monitor->connections
    = g_list_prepend (network_monitor->connections, c);

  g_signal_emit (c->network_monitor,
		 NC_NETWORK_MONITOR_GET_CLASS
		  (c->network_monitor)->new_connection_signal_id, 0, c);

  return c;
}

static void
nc_network_connection_dispose (GObject *object)
{
  NCNetworkConnection *c = NC_NETWORK_CONNECTION (object);

  debug (0, DEBUG_BOLD ("Disposing %s"), c->dbus_object);

  if (c->connected_at)
    printf ("%s connected %d seconds\n",
	    c->dbus_object,
	    (int) (time (NULL) - c->connected_at));

  /* Definately not the default connection any more.  Do this first so
     that all methods still work.  */
  default_connection_update (c, false);
  
  c->network_monitor->connections
    = g_list_remove (c->network_monitor->connections, c);

  /* Free the per connection device state.  */
  GList *n = c->devices;
  c->devices = NULL;
  while (n)
    {
      GList *e = n;
      n = e->next;

      g_free (e->data);
      g_list_free_1 (e);
    }

  if (c->connection_active_proxy)
    {
      g_object_unref (c->connection_active_proxy);
      c->connection_active_proxy = NULL;
    }

  g_free (c->dbus_object);
  c->dbus_object = NULL;

  /* Chain up to the parent class */
  G_OBJECT_CLASS (nc_network_connection_parent_class)->dispose (object);
}

uint32_t
nc_network_connection_mediums (NCNetworkConnection *c)
{
  uint32_t mediums = 0;
  GList *e;
  for (e = c->devices; e; e = e->next)
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
  /* Resolve dependencies.  */
  if ((mask & NC_DEVICE_INFO_STATS))
    stats_update (c->network_monitor);

  GList *ret = NULL;

  GList *e;
  for (e = c->devices; e; e = e->next)
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
		 e->data, c->dbus_object);
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
	     

      if (mask & (NC_DEVICE_INFO_IP_IP4_ADDR
		  | NC_DEVICE_INFO_IP_IP6_ADDR
		  | NC_DEVICE_INFO_GATEWAY_IP4_ADDR
		  | NC_DEVICE_INFO_GATEWAY_IP6_ADDR
		  | NC_DEVICE_INFO_GATEWAY_MAC_ADDR))
	{
	  if (! d->ip4_config_proxy)
	    {
	      assert (! d->ip4_config_dbus_object);
	      d->ip4_config_dbus_object
		= dbus_property_lookup_str (d->device_proxy, NULL, NULL,
					    "Ip4Config");

	      debug (0, "%s's Ip4Config: %s",
		     d->dbus_object, d->ip4_config_dbus_object);

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

	  if ((mask & NC_DEVICE_INFO_GATEWAY_MAC_ADDR)
	      && (info->mask & NC_DEVICE_INFO_IP_IP4_ADDR))
	    {
	      FILE *f = fopen ("/proc/net/arp", "r");
	      if (! f)
		debug (0, "Failed to open /proc/net/arp: %m");
	      else
		{
		  char ip[4 * 4 + 1];
		  int ip_len = sprintf (ip, "%d.%d.%d.%d ",
					info->gateway4[0],
					info->gateway4[1],
					info->gateway4[2],
					info->gateway4[3]);

		  char *line = NULL;
		  size_t n = 0;
		  int l;

		  while ((l = getline (&line, &n, f)) != -1)
		    if (strncmp (ip, line, ip_len) == 0)
		      {
			/* A line has the format:

			   IP Address, HW type, Flags, HW address, Mask, Device

			   We need the HW address.  We split on the
			   spaces.
			 */

			/* Kill the trailing newline.  */
			// Only needed if we need the last word
			// line[l - 1] = 0;

			char *e[3];
			int i;
			char *p;
			for (i = 0, p = line + ip_len;
			     *p && i < sizeof (e) / sizeof (*e);
			     i ++)
			  {
			    /* Skip spaces.  */
			    while (*p && *p == ' ')
			      {
				*p = 0;
				p ++;
			      }

			    e[i] = p;

			    /* Skip text.  */
			    while (*p && *p != ' ')
			      p ++;
			  }

			int a[6];
			sscanf (e[2], "%x:%x:%x:%x:%x:%x",
				&a[0], &a[1], &a[2],
				&a[3], &a[4], &a[5]);

			for (i = 0; i < 6; i ++)
			  info->gateway_hwaddr[i] = a[i];

			info->mask |= NC_DEVICE_INFO_GATEWAY_MAC_ADDR;

			break;
		      }

		  free (line);
		}

	      fclose (f);
	    }
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

bool
nc_network_connection_is_default (NCNetworkConnection *c)
{
  return dbus_property_lookup_int
    (c->connection_active_proxy, NULL, NULL, "Default", false);
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

static const char *
nm_device_type_to_str (int type)
{
  switch (type)
    {
#if HAVE_NETWORK_MANAGER
    case NM_DEVICE_TYPE_ETHERNET:
      return "ethernet";
    case NM_DEVICE_TYPE_WIFI:
      return "wifi";
    case NM_DEVICE_TYPE_GSM:
      return "gsm";
    case NM_DEVICE_TYPE_CDMA:
      return "cdma";
    case NM_DEVICE_TYPE_BT:
      return "blue tooth";
    case NM_DEVICE_TYPE_OLPC_MESH:
      return "OLPC mesh";
    case NM_DEVICE_TYPE_UNKNOWN:
#endif
    default:
      return "unknown";
    }
}

static const char *
nm_device_state_to_str (int type)
{
  switch (type)
    {
#if HAVE_NETWORK_MANAGER
    case NM_DEVICE_STATE_UNMANAGED:
      return "unmanaged";
    case NM_DEVICE_STATE_UNAVAILABLE:
      return "unavailable";
    case NM_DEVICE_STATE_DISCONNECTED:
      return "disconnected";
    case NM_DEVICE_STATE_PREPARE:
      return "prepare";
    case NM_DEVICE_STATE_CONFIG:
      return "config";
    case NM_DEVICE_STATE_NEED_AUTH:
      return "need auth";
    case NM_DEVICE_STATE_IP_CONFIG:
      return "ip config";
    case NM_DEVICE_STATE_ACTIVATED:
      return "activated";
    case NM_DEVICE_STATE_FAILED:
      return "failed";
    case NM_DEVICE_STATE_UNKNOWN:
#endif
    default:
      return "unknown";
    }
}

static const char *
nm_device_state_change_reason_to_str (int reason)
{
  switch (reason)
    {
    default:
      return "Reason code unknown.";
#if HAVE_NETWORK_MANAGER
    case NM_DEVICE_STATE_REASON_NONE:
      return "No reason given";
    case NM_DEVICE_STATE_REASON_UNKNOWN:
      return "Unknown error";
    case NM_DEVICE_STATE_REASON_NOW_MANAGED:
      return "Device is now managed";
    case NM_DEVICE_STATE_REASON_NOW_UNMANAGED:
      return "Device is now managed unmanaged";
    case NM_DEVICE_STATE_REASON_CONFIG_FAILED:
      return "The device could not be readied for configuration";
    case NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE:
      return "IP configuration could not be reserved (no available address, timeout, etc)";
    case NM_DEVICE_STATE_REASON_IP_CONFIG_EXPIRED:
      return "The IP config is no longer valid";
    case NM_DEVICE_STATE_REASON_NO_SECRETS:
      return "Secrets were required, but not provided";
    case NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT:
      return "802.1x supplicant disconnected";
    case NM_DEVICE_STATE_REASON_SUPPLICANT_CONFIG_FAILED:
      return "802.1x supplicant configuration failed";
    case NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED:
      return "802.1x supplicant failed";
    case NM_DEVICE_STATE_REASON_SUPPLICANT_TIMEOUT:
      return "802.1x supplicant took too long to authenticate";
    case NM_DEVICE_STATE_REASON_PPP_START_FAILED:
      return "PPP service failed to start";
    case NM_DEVICE_STATE_REASON_PPP_DISCONNECT:
      return "PPP service disconnected";
    case NM_DEVICE_STATE_REASON_PPP_FAILED:
      return "PPP failed";
    case NM_DEVICE_STATE_REASON_DHCP_START_FAILED:
      return "DHCP client failed to start";
    case NM_DEVICE_STATE_REASON_DHCP_ERROR:
      return "DHCP client error";
    case NM_DEVICE_STATE_REASON_DHCP_FAILED:
      return "DHCP client failed";
    case NM_DEVICE_STATE_REASON_SHARED_START_FAILED:
      return "Shared connection service failed to start";
    case NM_DEVICE_STATE_REASON_SHARED_FAILED:
      return "Shared connection service failed";
    case NM_DEVICE_STATE_REASON_AUTOIP_START_FAILED:
      return "AutoIP service failed to start";
    case NM_DEVICE_STATE_REASON_AUTOIP_ERROR:
      return "AutoIP service error";
    case NM_DEVICE_STATE_REASON_AUTOIP_FAILED:
      return "AutoIP service failed";
    case NM_DEVICE_STATE_REASON_MODEM_BUSY:
      return "The line is busy";
    case NM_DEVICE_STATE_REASON_MODEM_NO_DIAL_TONE:
      return "No dial tone";
    case NM_DEVICE_STATE_REASON_MODEM_NO_CARRIER:
      return "No carrier could be established";
    case NM_DEVICE_STATE_REASON_MODEM_DIAL_TIMEOUT:
      return "The dialing request timed out";
    case NM_DEVICE_STATE_REASON_MODEM_DIAL_FAILED:
      return "The dialing attempt failed";
    case NM_DEVICE_STATE_REASON_MODEM_INIT_FAILED:
      return "Modem initialization failed";
    case NM_DEVICE_STATE_REASON_GSM_APN_FAILED:
      return "Failed to select the specified APN";
    case NM_DEVICE_STATE_REASON_GSM_REGISTRATION_NOT_SEARCHING:
      return "Not searching for networks";
    case NM_DEVICE_STATE_REASON_GSM_REGISTRATION_DENIED:
      return "Network registration denied";
    case NM_DEVICE_STATE_REASON_GSM_REGISTRATION_TIMEOUT:
      return "Network registration timed out";
    case NM_DEVICE_STATE_REASON_GSM_REGISTRATION_FAILED:
      return "Failed to register with the requested network";
    case NM_DEVICE_STATE_REASON_GSM_PIN_CHECK_FAILED:
      return "PIN check failed";
    case NM_DEVICE_STATE_REASON_FIRMWARE_MISSING:
      return "Necessary firmware for the device may be missing";
    case NM_DEVICE_STATE_REASON_REMOVED:
      return "The device was removed";
    case NM_DEVICE_STATE_REASON_SLEEPING:
      return "NetworkManager went to sleep";
    case NM_DEVICE_STATE_REASON_CONNECTION_REMOVED:
      return "The device's active connection disappeared";
    case NM_DEVICE_STATE_REASON_USER_REQUESTED:
      return "Device disconnected by user or client";
    case NM_DEVICE_STATE_REASON_CARRIER:
      return "Carrier/link changed";
    case NM_DEVICE_STATE_REASON_CONNECTION_ASSUMED:
      return "The device's existing connection was assumed";
    case NM_DEVICE_STATE_REASON_SUPPLICANT_AVAILABLE:
      return "The supplicant is now available";
#endif
    }
}

/* Scan for new and stale connections.  It is essential that this be
   done in an idle callback.  Consider the case where two devices are
   added in quick succession.  */
static gboolean
active_connections_scan_cb (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  /* Check if there are any new connections.  This is non-trivial
     because a connection can be associated with multiple devices.
     Thus, we list all of the current active connections and figure
     which are new and which are stale.  */
  static GType object_path_array_type;
  if (! object_path_array_type)
    object_path_array_type
      = dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH);

  GValue connections_value = { 0 };
  if (! dbus_property_lookup
      (m->network_manager_proxy, NULL, NULL,
       "ActiveConnections", object_path_array_type, &connections_value))
    {
      printf ("Failed to look up "
	      "org.freedesktop.NetworkManager.ActiveConnections\n");
      goto out;
    }

  /* Mark all current connections as being unseen.  */
  GList *e;
  for (e = m->connections; e; e = e->next)
    {
      NCNetworkConnection *c = NC_NETWORK_CONNECTION (e->data);
      c->seen = false;
    }

  GPtrArray *connections = g_value_get_boxed (&connections_value);
  int i = 0;
  while (i < connections->len)
    {
      char *connection_dbus_object = g_ptr_array_index (connections, i);

      for (e = m->connections; e; e = e->next)
	{
	  NCNetworkConnection *c = NC_NETWORK_CONNECTION (e->data);
	  if (strcmp (c->dbus_object, connection_dbus_object) == 0)
	    {
	      debug (0, "Connection %s known.", connection_dbus_object);

	      c->seen = true;
	      g_ptr_array_remove_index_fast (connections, i);

	      break;
	    }
	}

      if (! e)
	{
	  /* Only increment I if we haven't removed the element at
	     I.  */
	  debug (0, "Connection %s is new.", connection_dbus_object);
	  i ++;
	}
    }

  /* Any "unseen" connections are stale.  Disconnect them.  Any
     connections remaining in CONNECTIONS are new.  Notice them.  */

  /* First do the disconnects.  */
  GList *n = m->connections;
  while (n)
    {
      e = n;
      n = e->next;

      NCNetworkConnection *c = NC_NETWORK_CONNECTION (e->data);
      if (! c->seen)
	{
	  g_signal_emit (m,
			 NC_NETWORK_MONITOR_GET_CLASS (m)
			   ->disconnected_signal_id,
			 0, c);

	  g_object_unref (c);
	}
    }

  /* Then, do the connects.  */
  for (i = 0; i < connections->len; i ++)
    {
      char *connection_dbus_object = g_ptr_array_index (connections, i);

      NCNetworkConnection *c;
      c = nc_network_connection_new (m, connection_dbus_object);
      if (nc_network_connection_is_default (c))
	default_connection_update (c, true);
    }

 out:
  g_value_unset (&connections_value);

  m->active_connections_scan_pending_id = 0;

  return false;
}

static void
device_state_changed_cb (DBusGProxy *proxy,
			 uint32_t nstate, uint32_t ostate, uint32_t reason,
			 gpointer user_data)
{
  /* This function is not multi-threaded safe nor is it reentrant!  */
  static bool in;
  assert (! in);
  in = true;

  NCNetworkDevice *d = NC_NETWORK_DEVICE (user_data);
  assert (d->device_proxy == proxy);

  printf ("%s's state changed: %s -> %s: %s\n",
	  dbus_g_proxy_get_path (proxy),
	  nm_device_state_to_str (ostate), nm_device_state_to_str (nstate),
	  nm_device_state_change_reason_to_str (reason));

  /* Whatever we do, first hang up any connection-dependent data.  */

  if (d->access_point_proxy)
    {
      g_object_unref (d->access_point_proxy);
      d->access_point_proxy = NULL;

      g_free (d->access_point_dbus_object);
      d->access_point_dbus_object = NULL;

      g_free (d->ssid);
      d->ssid = NULL;
    }

  if (d->ip4_config_dbus_object)
    {
      g_object_unref (d->ip4_config_proxy);
      d->ip4_config_proxy = NULL;

      g_free (d->ip4_config_dbus_object);
      d->ip4_config_dbus_object = NULL;
    }

  /* Transition to the new state.  */
  d->state = nstate;

  /* If the device has been activated and the device is a WiFi device,
     we likely are associated with an access point.  */
  if (d->state == NM_DEVICE_STATE_ACTIVATED
      && d->medium == NC_CONNECTION_MEDIUM_WIFI)
    {
      d->access_point_dbus_object = dbus_property_lookup_str
	(d->device_proxy, d->dbus_object,
	 "org.freedesktop.NetworkManager.Device.Wireless",
	 "ActiveAccessPoint");

      d->access_point_proxy = dbus_g_proxy_new_from_proxy
	(d->device_proxy,
	 "org.freedesktop.NetworkManager.AccessPoint",
	 d->access_point_dbus_object);

      d->ssid = dbus_property_lookup_str
	(d->access_point_proxy, NULL, NULL, "Ssid");
    }

  if (! d->network_monitor->active_connections_scan_pending_id)
    d->network_monitor->active_connections_scan_pending_id
      = g_idle_add (active_connections_scan_cb, d->network_monitor);

  assert (in);
  in = false;
}

static void
nc_network_device_init (NCNetworkDevice *d)
{
}

static NCNetworkDevice *
nc_network_device_new (NCNetworkMonitor *network_monitor,
		       const char *dbus_object_device)
{
  debug (0, "New device: %s", dbus_object_device);

  NCNetworkDevice *d
    = NC_NETWORK_DEVICE (g_object_new (NC_NETWORK_DEVICE_TYPE, NULL));

  d->network_monitor = network_monitor;
  network_monitor->devices = g_list_prepend (network_monitor->devices, d);

  d->dbus_object = g_strdup (dbus_object_device);

  d->device_proxy = dbus_g_proxy_new_from_proxy
    (network_monitor->network_manager_proxy,
     "org.freedesktop.NetworkManager.Device", d->dbus_object);

  dbus_g_proxy_add_signal (d->device_proxy, "StateChanged",
			   G_TYPE_UINT,
			   G_TYPE_UINT,
			   G_TYPE_UINT,
			   G_TYPE_INVALID);

  dbus_g_proxy_connect_signal (d->device_proxy,
			       "StateChanged",
			       G_CALLBACK (device_state_changed_cb),
			       d, NULL);

  d->medium = dbus_property_lookup_int
    (d->device_proxy, NULL, NULL, "DeviceType", NM_DEVICE_TYPE_UNKNOWN);
  switch (d->medium)
    {
    default:
      d->medium = NC_CONNECTION_MEDIUM_UNKNOWN;
      break;
    case NM_DEVICE_TYPE_ETHERNET:
      d->medium = NC_CONNECTION_MEDIUM_ETHERNET;
      break;
    case NM_DEVICE_TYPE_WIFI:
    case NM_DEVICE_TYPE_OLPC_MESH:
      d->medium = NC_CONNECTION_MEDIUM_WIFI;
      break;
    case NM_DEVICE_TYPE_GSM:
    case NM_DEVICE_TYPE_CDMA:
      d->medium = NC_CONNECTION_MEDIUM_CELLULAR;
      break;
    case NM_DEVICE_TYPE_BT:
      d->medium = NC_CONNECTION_MEDIUM_BLUETOOTH;
      break;
    }

  d->state = dbus_property_lookup_int
    (d->device_proxy, NULL, NULL, "State", NM_DEVICE_STATE_UNKNOWN);

  d->interface = dbus_property_lookup_str
    (d->device_proxy, NULL, NULL, "Interface");

  debug (0, "  Medium: %s (%d)",
	  nm_device_type_to_str (d->medium), d->medium);
  debug (0, "  State: %s (%d)",
	 nm_device_state_to_str (d->state), d->state);

  device_state_changed_cb (d->device_proxy,
			   d->state, NM_DEVICE_STATE_UNKNOWN,
			   NM_DEVICE_STATE_REASON_NONE, d);

  return d;
}

static void
nc_network_device_dispose (GObject *object)
{
  NCNetworkDevice *d = NC_NETWORK_DEVICE (object);

  debug (0, DEBUG_BOLD ("Disposing device %s"), d->dbus_object);

  g_free (d->interface);
  d->interface = NULL;

  if (d->network_monitor)
    {
      device_state_changed_cb (d->device_proxy,
			       NM_DEVICE_STATE_DISCONNECTED,
			       d->state, NM_DEVICE_STATE_REASON_NONE,
			       d);

      NCNetworkMonitor *m = d->network_monitor;

      GList *e = g_list_find (m->devices, d);
      assert (e);
      m->devices = g_list_delete_link (m->devices, e);

      d->network_monitor = NULL;
    }

  d->network_monitor->devices
    = g_list_remove (d->network_monitor->devices, d);

  g_free (d->dbus_object);
  d->dbus_object = NULL;

  if (d->device_proxy)
    {
      g_object_unref (d->device_proxy);
      d->device_proxy = NULL;
    }

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

  dbus_g_object_register_marshaller
    (g_cclosure_user_marshal_VOID__UINT_UINT_UINT,
     G_TYPE_NONE, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT,
     G_TYPE_INVALID);

  dbus_g_object_register_marshaller
    (g_cclosure_user_marshal_VOID__STRING,
     G_TYPE_NONE, G_TYPE_STRING,
     G_TYPE_INVALID);

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
}

/* A new device was added.  */
static void
device_added_cb (DBusGProxy *proxy, const char *device, gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);
  nc_network_device_new (m, device);
}

/* A device was removed (e.g., unplugged), destroy its corresponding
   NCNetworkDevice object.  */
static void
device_removed_cb (DBusGProxy *proxy, const char *device, gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  printf ("%s removed: ", device);

  NCNetworkDevice *d = device_dbus_to_device (m, device);
  if (d)
    {
      /* XXX: Check if any connections reference this device.
	 (Which shouldn't be the case, but...)  */
      g_object_unref (d);
      printf ("ok.\n");
    }
  else
    printf ("not known.\n");
}

/* Enumerate all network devices, create corresponding local objects
   and start listening for state changes.  */
static gboolean
start (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  printf ("Listing devices.\n");

  GPtrArray *devices = NULL;
  GError *error = NULL;
  if (! org_freedesktop_NetworkManager_get_devices
          (m->network_manager_proxy, &devices, &error))
    {
      if (error->domain == DBUS_GERROR
	  && error->code == DBUS_GERROR_REMOTE_EXCEPTION)
        g_printerr ("Caught remote method exception %s: %s",
	            dbus_g_error_get_name (error),
	            error->message);
      else
        g_printerr ("Error: %s\n", error->message);
      g_error_free (error);
    }
  else
    {
      int i;
      for (i = 0; i < devices->len; i ++)
	{
	  char *device = (char *) g_ptr_array_index (devices, i);
	  printf ("%s\n", device);

	  device_added_cb (m->network_manager_proxy, device, m);
	}

      g_ptr_array_free (devices, TRUE);
    }

  /* Don't run this idle handler again.  */
  return false;
}

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

  m->network_manager_proxy = dbus_g_proxy_new_for_name
    (m->system_bus,
     "org.freedesktop.NetworkManager",
     "/org/freedesktop/NetworkManager",
     "org.freedesktop.NetworkManager");

  /* Query devices and extant connections the next time things are
     idle.  */
  g_idle_add (start, m);

  dbus_g_proxy_add_signal (m->network_manager_proxy, "DeviceAdded",
			   G_TYPE_STRING,
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (m->network_manager_proxy, "DeviceAdded",
			       G_CALLBACK (device_added_cb), m, NULL);

  dbus_g_proxy_add_signal (m->network_manager_proxy, "DeviceRemoved",
			   G_TYPE_STRING,
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (m->network_manager_proxy, "DeviceRemoved",
			       G_CALLBACK (device_removed_cb), m, NULL);
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
