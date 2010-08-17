/* network-monitor-nm.c - Network Manager network monitor backend.
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

static const char *
device_state_to_str (int state)
{
  switch (state)
    {
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
    default:
      return "unknown";
    }
}

static const char *
connection_state_to_str (int state)
{
  switch (state)
    {
    case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
      return "activating";
    case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
      return "activated";
    default:
      return "unknown";
    }
}

static bool
connection_state_is_connected (int state)
{
  return state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED;
}

static bool
device_state_is_connected (int state)
{
  return state == NM_DEVICE_STATE_ACTIVATED;
}

static void
default_connection_scan (NCNetworkMonitor *m)
{
}

/* Detect changes to a connection's properties.  In particular, detect
   changes to the default route.  */
static void
connection_connection_active_properties_changed_cb (DBusGProxy *proxy,
						    GHashTable *properties,
						    NCNetworkConnection *c)
{
  assert (c->connection_active_proxy == proxy);

  void iter (gpointer key, gpointer data, gpointer user_data)
  {
    GValue *value = data;

    debug (0, "%s: key %s changed", c->name, key);

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
	    default_connection_update (c->network_monitor, c, set);
	  }
      }
    else if (strcmp (key, "State") == 0)
      /* The connection's state changed.  */
      {
	if (G_VALUE_TYPE (value) != G_TYPE_UINT)
	  {
	    debug (0, "%s's type should be uint but got %s",
		   key, g_type_name (G_VALUE_TYPE (value)));
	  }
	else
	  connection_state_set (c, g_value_get_uint (value), false);
      }
  }

  g_hash_table_foreach (properties, iter, NULL);
}

static void
nc_network_connection_backend_new (NCNetworkConnection *c)
{
  c->connection_active_proxy = dbus_g_proxy_new_from_proxy
    (c->network_monitor->network_manager_proxy,
     "org.freedesktop.NetworkManager.Connection.Active",
     c->name);

  dbus_g_proxy_add_signal (c->connection_active_proxy,
			   "PropertiesChanged",
			   DBUS_TYPE_G_MAP_OF_VARIANT,
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal
    (c->connection_active_proxy, "PropertiesChanged",
     G_CALLBACK (connection_connection_active_properties_changed_cb),
     c, NULL);

  if (dbus_property_lookup_int
      (c->connection_active_proxy, NULL, NULL, "Default", false))
    default_connection_update (c->network_monitor, c, true);
}

/* Connection and device state management.  */

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
      debug (0, "Failed to look up "
	     "org.freedesktop.NetworkManager.ActiveConnections");
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
	  if (strcmp (c->name, connection_dbus_object) == 0)
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
	connection_state_set (c, CONNECTION_STATE_DISCONNECTED, false);
    }

  /* Then, do the connects.  */
  for (i = 0; i < connections->len; i ++)
    {
      char *connection_dbus_object = g_ptr_array_index (connections, i);
      NCNetworkConnection *c
	= nc_network_connection_new (m, connection_dbus_object);

      static GType object_path_array_type;
      if (! object_path_array_type)
	object_path_array_type
	  = dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH);

      GValue devices_value = { 0 };
      if (! dbus_property_lookup
	  (c->connection_active_proxy, NULL, NULL, "Devices",
	   object_path_array_type, &devices_value))
	{
	  debug (0, "Failed to look up "
		 "org.freedesktop.NetworkManager.ActiveConnections.Devices");
	}
      else
	{
	  debug (0, "Connection %s uses devices:", c->name);

	  void cb (gpointer data, gpointer user_data)
	  {
	    char *device_dbus_object = data;
	    debug (0, "  %s", device_dbus_object);
	    connection_add_device (c, device_dbus_object);
	  }
	  GPtrArray *devices = g_value_get_boxed (&devices_value);
	  g_ptr_array_foreach (devices, cb, NULL);
	}

      int state
	= dbus_property_lookup_int (c->connection_active_proxy, NULL, NULL,
				    "State", CONNECTION_STATE_DISCONNECTED);
      connection_state_set (c, state, true);
    }

 out:
  g_value_unset (&connections_value);

  m->active_connections_scan_pending_id = 0;

  return false;
}

static const char *
nm_device_state_change_reason_to_str (int reason)
{
  switch (reason)
    {
    default:
      return "Reason code unknown.";
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
    }
}

static void
nm_device_state_changed_cb (DBusGProxy *proxy,
			    uint32_t nstate, uint32_t ostate, uint32_t reason,
			    gpointer user_data)
{
  NCNetworkDevice *d = NC_NETWORK_DEVICE (user_data);
  assert (d->device_proxy == proxy);

  debug (0, "%s's state changed: %s -> %s: %s",
	 dbus_g_proxy_get_path (proxy),
	 device_state_to_str (ostate), device_state_to_str (nstate),
	 nm_device_state_change_reason_to_str (reason));

  device_state_changed (d, nstate);
}

static const char *
nm_device_type_to_str (int type)
{
  switch (type)
    {
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
    default:
      return "unknown";
    }
}

/* A new device was added.  */
static void
device_added_cb (DBusGProxy *proxy, const char *device, gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  DBusGProxy *device_proxy = dbus_g_proxy_new_from_proxy
    (m->network_manager_proxy,
     "org.freedesktop.NetworkManager.Device", device);

  char *interface = dbus_property_lookup_str
    (device_proxy, NULL, NULL, "Interface");

  int nm_medium = dbus_property_lookup_int
    (device_proxy, NULL, NULL, "DeviceType", NM_DEVICE_TYPE_UNKNOWN);

  debug (0, "NM Medium: %s", nm_device_type_to_str (nm_medium));

  int medium;
  switch (nm_medium)
    {
    default:
      medium = NC_CONNECTION_MEDIUM_UNKNOWN;
      break;
    case NM_DEVICE_TYPE_ETHERNET:
      medium = NC_CONNECTION_MEDIUM_ETHERNET;
      break;
    case NM_DEVICE_TYPE_WIFI:
    case NM_DEVICE_TYPE_OLPC_MESH:
      medium = NC_CONNECTION_MEDIUM_WIFI;
      break;
    case NM_DEVICE_TYPE_GSM:
    case NM_DEVICE_TYPE_CDMA:
      medium = NC_CONNECTION_MEDIUM_CELLULAR;
      break;
    case NM_DEVICE_TYPE_BT:
      medium = NC_CONNECTION_MEDIUM_BLUETOOTH;
      break;
    }

  int state = dbus_property_lookup_int
    (device_proxy, NULL, NULL, "State", NM_DEVICE_STATE_UNKNOWN);

  debug (0, "  State: %s (%d)",
	 device_state_to_str (state), state);

  NCNetworkDevice *d
    = nc_network_device_new (m, device, interface, medium);

  d->device_proxy = dbus_g_proxy_new_from_proxy
    (d->network_monitor->network_manager_proxy,
     "org.freedesktop.NetworkManager.Device", d->name);

  dbus_g_object_register_marshaller
    (g_cclosure_user_marshal_VOID__UINT_UINT_UINT,
     G_TYPE_NONE, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT,
     G_TYPE_INVALID);
  dbus_g_proxy_add_signal (d->device_proxy, "StateChanged",
			   G_TYPE_UINT,
			   G_TYPE_UINT,
			   G_TYPE_UINT,
			   G_TYPE_INVALID);

  dbus_g_proxy_connect_signal (d->device_proxy,
			       "StateChanged",
			       G_CALLBACK (nm_device_state_changed_cb),
			       d, NULL);

  device_state_changed (d, state);
}

/* A device was removed (e.g., unplugged), destroy its corresponding
   NCNetworkDevice object.  */
static void
device_removed_cb (DBusGProxy *proxy, const char *device, gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  debug (0, "%s removed: ", device);

  NCNetworkDevice *d = device_name_to_device (m, device);
  if (d)
    {
      /* XXX: Check if any connections reference this device.
	 (Which shouldn't be the case, but...)  */
      g_object_unref (d);
      debug (0, "ok.");
    }
  else
    debug (0, "not known.");
}

/* Enumerate all network devices, create corresponding local objects
   and start listening for state changes.  */
static gboolean
start (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  debug (0, "Listing devices.");

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
	  debug (0, "%s", device);

	  device_added_cb (m->network_manager_proxy, device, m);
	}

      g_ptr_array_free (devices, TRUE);
    }

  /* Don't run this idle handler again.  */
  return false;
}

static void
nc_network_monitor_backend_init (NCNetworkMonitor *m)
{
  m->network_manager_proxy = dbus_g_proxy_new_for_name
    (m->system_bus,
     "org.freedesktop.NetworkManager",
     "/org/freedesktop/NetworkManager",
     "org.freedesktop.NetworkManager");

  dbus_g_object_register_marshaller
    (g_cclosure_user_marshal_VOID__STRING,
     G_TYPE_NONE, G_TYPE_STRING,
     G_TYPE_INVALID);

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

  /* Query devices and extant connections the next time things are
     idle.  */
  g_idle_add (start, m);
}
