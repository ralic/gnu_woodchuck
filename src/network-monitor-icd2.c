/* network-monitor-icd2.c - ICD2 network monitor backend.
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

/* Implementation notes:

   We do not garbage collect devices.  This means if devices are
   removed (or not in use), an object remains allocated.  In practice,
   this shouldn't be a problem as there are at most a few devices.
 */

static const char *
device_state_to_str (int state)
{
  switch (state)
    {
    case ICD_STATE_DISCONNECTED:
      return "disconnected";
    case ICD_STATE_CONNECTING:
      return "connecting";
    case ICD_STATE_CONNECTED:
      return "connected";
    case ICD_STATE_DISCONNECTING:
      return "disconnecting";
    case ICD_STATE_LIMITED_CONN_ENABLED:
      return "limited connectivity enabled";
    case ICD_STATE_LIMITED_CONN_DISABLED:
      return "limited connectivity disabled";
    case ICD_STATE_SEARCH_START:
      return "search started";
    case ICD_STATE_SEARCH_STOP:
      return "search stopped";
    case ICD_STATE_INTERNAL_ADDRESS_ACQUIRED:
      return "internal address acquired";
    default:
      return "unknown";
    }
}

static const char *
connection_state_to_str (int state)
{
  return device_state_to_str (state);
}

static bool
connection_state_is_connected (int state)
{
  return state == ICD_STATE_CONNECTED
    || state == ICD_STATE_LIMITED_CONN_ENABLED
    || state == ICD_STATE_LIMITED_CONN_DISABLED
    || state == ICD_STATE_SEARCH_START
    || state == ICD_STATE_SEARCH_STOP;
}

static bool
device_state_is_connected (int state)
{
  return connection_state_is_connected (state);
}

static gboolean
default_connection_scan_cb (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  NCNetworkConnection *default_connection = NULL;

  bool route_cb (char *interface, char *rest)
  {
    char *fields[7];
    int count = split_line (rest,
			    sizeof (fields) / sizeof (fields[0]),
			    fields);
    if (count != sizeof (fields) / sizeof (fields[0]))
      {
	debug (0, "Misformed line! Got %d fields!", count);
	return true;
      }

    uint32_t flags = strtol (fields[2], NULL, 16);
    uint32_t dest = strtol (fields[0], NULL, 16);
    uint32_t mask = strtol (fields[6], NULL, 16);
    /* If the default route flag is set or the dest is 0.0.0.0 and the
       mask is 0.0.0.0, we've found the default gateway.  */
    if (! ((flags & 0x2) || (dest == 0 && mask == 0)))
      return true;

    NCNetworkDevice *d = device_interface_to_device (m, interface);
    if (! d)
      /* Not managed by us.  */
      {
	debug (0, "Ignoring device %s with default route, not known to us.",
	       interface);
	return false;
      }

    int dl = 5;

    /* Find the connection using this device.  */
    GList *e;
    for (e = m->connections; e; e = e->next)
      {
	NCNetworkConnection *c = NC_NETWORK_CONNECTION (e->data);

	debug (dl, "Checking if connection %s uses %s",
	       c->name, d->interface);

	GList *f;
	for (f = c->per_connection_device_state; f; f = f->next)
	  {
	    struct per_connection_device_state *cd = f->data;
	    NCNetworkDevice *d2 = per_connection_device_to_device (c, cd);

	    debug (dl, "  uses %s", d2->interface);

	    if (d == d2)
	      {
		debug (dl, "Default device %s belongs to connection %s",
		       interface, c->name);
		if (connection_state_is_connected (c->state))
		  /* If it is not connected, it is likely
		     disconnecting, but not completely gone.  */
		  default_connection = c;
		else
		  debug (0, "Default connection (%s) is disconnected!",
			 c->name);

		return false;
	      }
	  }
      }

    debug (0, "No connection associated with device %s", interface);

    /* We're done.  */
    return false;
  }
  for_each_proc_net_route (route_cb);

  default_connection_update
    (m, default_connection,
     (default_connection
      ? connection_state_is_connected (default_connection->state)
      : true));

  g_source_remove (m->default_connection_scan_source);
  m->default_connection_scan_source = 0;

  /* Don't run again.  */
  return false;
}

static void
default_connection_scan (NCNetworkMonitor *m)
{
  if (! m->default_connection_scan_source)
    m->default_connection_scan_source
      = g_idle_add (default_connection_scan_cb, m);
}

static void
nc_network_connection_backend_new (NCNetworkConnection *c)
{
}

/* Connection and device state management.  */

static GType assssss;

static gboolean
state_req (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  unsigned int count = 0;
  GError *e = NULL;
  if (! com_nokia_icd2_state_req (m->icd2_proxy, &count, &e))
    {
      debug (0, "Error invoking state_req: %s", e->message);
      g_error_free (e);
    }

  m->state_req_source = 0;

  /* Don't run again.  */
  return false;
}

static void
addrinfo_sig_cb (DBusGProxy *proxy,
		 char *service_type,
		 uint32_t service_attributes,
		 char *service_id,
		 char *network_type,
		 uint32_t network_attributes,
		 GArray *network_id_array,
		 GPtrArray *addresses,
		 gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  char network_id[network_id_array->len + 1];
  memcpy (network_id, network_id_array->data, network_id_array->len);
  network_id[network_id_array->len] = 0;
  debug (0, "service: %s/%x/%s; network: %s/%x/%s",
	 service_type, service_attributes, service_id,
	 network_type, network_attributes, network_id);

  NCNetworkConnection *c = connection_name_to_connection (m, network_id);
  if (! c)
    {
      if (! m->state_req_source)
	m->state_req_source = g_idle_add (state_req, m);
      return;
    }

  bool new_connection = false;
  if (c->per_connection_device_state == NULL)
    /* This is a new connection: we haven't assign it devices yet.  */
    {
      debug (0, DEBUG_BOLD ("New connection %s"), network_id);
      new_connection = true;
    }

  /* XXX: What should we do if there is more than one address?  Does
     it mean that there is a device stack (e.g., vpn over wireless) or
     does it mean that one device has multiple ip addresses?
     Currently, we assume the former.  But in that case, what does
     NETWORK_TYPE really mean?  */
  debug (0, "%s has %d addresses", network_id, addresses->len);
  int i;
  for (i = 0; i < addresses->len; i ++)
    {
      GValueArray *boxed_info = g_ptr_array_index (addresses, i);

      do_debug (5)
	{
	  char *desc[] = { "ip", "netmask", "gateway",
			   "nameserver1", "nameserver2", "nameserver3" };
	  int j;
	  for (j = 0; j < 6; j ++)
	    {
	      GValue *ip = g_value_array_get_nth (boxed_info, j);
	      debug (0, "  %s: %s",
		     desc[j],
		     ip ? g_value_get_string (ip) : "<out of range>");
	    }
	}

      GValue *ip_value = g_value_array_get_nth (boxed_info, 0);
      const char *ip = NULL;
      if (ip_value)
	ip = g_value_get_string (ip_value);

      char *interface = NULL;
      if (ip)
	{
	  struct in_addr addr;
	  if (inet_aton (ip, &addr) == 0)
	    debug (0, "Failed to parse alleged ip address '%s'", ip);
	  else
	    {
	      interface = ip_to_interface (addr.s_addr);
	      debug (0, "Interface: %s", interface);
	    }
	}

      GList *e;
      NCNetworkDevice *d = NULL;
      for (e = m->devices; e; e = e->next)
	{
	  d = NC_NETWORK_DEVICE (e->data);
	  if (strcmp (d->interface, interface) == 0)
	    /* Already have device.  */
	    break;
	}
      if (! e)
	/* New device.  */
	{
	  /* ICD2 tells us the type of network connection but if a
	     connection consists of multiple devices, what is each
	     device?  We can figure out WiFi pretty easily, but the
	     rest... */
	  int medium = NC_CONNECTION_MEDIUM_UNKNOWN;
	  if (interface_is_wifi (interface))
	    medium = NC_CONNECTION_MEDIUM_WIFI;

	  if (addresses->len == 1)
	    {
	      if (strncmp (network_type, "WLAN_", 5) == 0)
		medium = NC_CONNECTION_MEDIUM_WIFI;
	      else if (strncmp (network_type, "GPRS", 4) == 0
		       || strncmp (network_type, "WIMAX", 5) == 0)
		medium = NC_CONNECTION_MEDIUM_CELLULAR;
	      else if (strncmp (network_type, "DUN_", 4) == 0)
		/* XXX: I haven't seen this in the wild and I'm not sure
		   what it means... Could it mean that the N900 is
		   connected to a cellular network over bluetooth?  */
		medium = NC_CONNECTION_MEDIUM_BLUETOOTH;
	    }

	  d = nc_network_device_new (m, interface, interface, medium);
	  device_state_changed (d, DEVICE_STATE_CONNECTED);
	}
      else
	device_state_changed (d, DEVICE_STATE_CONNECTED);

      if (new_connection)
	{
	  debug (0, DEBUG_BOLD ("Adding device %s to connection %s"),
		 interface, network_id);
	  connection_add_device (c, interface);
	}

      g_free (interface);
    }

  if (new_connection && c->per_connection_device_state)
    connection_state_set (c, c->state, true);

  default_connection_scan (c->network_monitor);
}

static gboolean
addrinfo_req (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  unsigned int count = 0;
  GError *e = NULL;
  if (! com_nokia_icd2_addrinfo_req (m->icd2_proxy, &count, &e))
    {
      debug (0, "Error invoking addrinfo_req: %s", e->message);
      g_error_free (e);
    }

  m->addrinfo_req_source = 0;

  /* Don't run again.  */
  return false;
}

static void
icd2_state_sig_cb (DBusGProxy *proxy,
		   char *service_type,
		   uint32_t service_attributes,
		   char *service_id,
		   char *network_type,
		   uint32_t network_attributes,
		   GArray *network_id_array,
		   char *error,
		   uint32_t nstate,
		   gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  char network_id[network_id_array->len + 1];
  memcpy (network_id, network_id_array->data, network_id_array->len);
  network_id[network_id_array->len] = 0;

  NCNetworkConnection *c = connection_name_to_connection (m, network_id);

  debug (0, DEBUG_BOLD ("state_sig: ") "%s: %s -> %s",
	 network_id,
	 c ? connection_state_to_str (c->state) : "<new connection>",
	 connection_state_to_str (nstate));

  bool schedule_addrinfo = false;
  if (! c)
    /* New (well, unknown) connection.  */
    {
      if (connection_state_is_connected (nstate))
	{
	  debug (0, "Now tracking connection %s.", network_id);

	  c = nc_network_connection_new (m, network_id);
	  /* Don't use connection_state_set as we are not ready to
	     publish this connection.  */
	  c->state = nstate;

	  schedule_addrinfo = true;
	}
      else
	debug (0, "Ignoring disconnected connection %s.", network_id);
    }
  else if (c && c->state != nstate)
    {
      connection_state_set (c, nstate, false);
      /* This will do a bit of extra work as we might not need to
	 check for a configuration change, but it should not be too
	 much.  */
      schedule_addrinfo = true;
    }

  if (schedule_addrinfo && ! m->addrinfo_req_source)
    m->addrinfo_req_source = g_idle_add (addrinfo_req, m);
}

/* Process network scan results.

   According to
   http://maemo.org/api_refs/5.0/5.0-final/icd2/group__dbus__api.html

   DBUS_TYPE_UINT32              status, see icd_scan_status
   DBUS_TYPE_UINT32              timestamp when last seen
   DBUS_TYPE_STRING              service type
   DBUS_TYPE_STRING              service name
   DBUS_TYPE_UINT32              service attributes, see Service Provider API
   DBUS_TYPE_STRING              service id
   DBUS_TYPE_INT32               service priority within a service type
   DBUS_TYPE_STRING              network type
   DBUS_TYPE_STRING              network name
   DBUS_TYPE_UINT32              network attributes, see Network module API
   DBUS_TYPE_ARRAY (BYTE)        network id
   DBUS_TYPE_INT32               network priority for different network types
   DBUS_TYPE_INT32               signal strength/quality, 0 (none) - 10 (good)
   DBUS_TYPE_STRING              station id, e.g. MAC address or similar id
                                 you can safely ignore this argument
   DBUS_TYPE_INT32               signal value in dB; use signal strength above
                                 unless you know what you are doing

   stats, according to
   http://maemo.org/api_refs/5.0/5.0-final/icd2/group__dbus__api.html:

   ICD_SCAN_NEW 	the returned network was found

   ICD_SCAN_UPDATE      an existing network with better signal
                        strength is found, applications may
                        want to update any saved data
                        concerning signal strength

   ICD_SCAN_NOTIFY      other network details have been updated but
                        will not be stored by ICd2; normally
                        networks with this status are best
                        ignored

   ICD_SCAN_EXPIRE 	the returned network has expired

   ICD_SCAN_COMPLETE    this round of scanning is complete and a
                        new scan will be started after the
                        module scan timeout

   Network attributes:

   From
   http://maemo.org/api_refs/5.0/5.0-final/icd2/group__network__module__api.html

   ICD_NW_ATTR_ALWAYS_ONLINE   0x20000000

     Whether the connection attempt is done because of always online
     policy, manual connection attempts do not set this

   ICD_NW_ATTR_AUTOCONNECT   0x04000000

     Whether we have all required credentials to authenticate
     ourselves to the network automatically without any user
     interaction

   ICD_NW_ATTR_IAPNAME   0x01000000

     Type of network id; set for IAP name, unset for local id,
     e.g. WLAN SSID

   ICD_NW_ATTR_LOCALMASK   0x00FFFFFF

     Mask for network attribute local values, e.g. security
     settings, WLAN mode, etc. These values might be evaluated by
     relevant UI components

   ICD_NW_ATTR_SILENT   0x02000000

     UI and user interaction forbidden if set, allowed if unset 

  ICD_NW_ATTR_SRV_PROVIDER   0x10000000

    Whether this network always needs service provider support in
    order to get connected
 */
static void
icd2_scan_sig_cb (DBusGProxy *proxy,
		  uint32_t status,
		  uint32_t last_seen,
		  char *service_type,
		  char *service_name,
		  uint32_t service_attributes,
		  char *service_id,
		  int32_t service_priority,
		  char *network_type,
		  char *network_name,
		  uint32_t network_attributes,
		  GArray *network_id_array,
		  int32_t network_priority,
		  int32_t signal_strength,
		  char *station_id,
		  int32_t signal_strength_db,
		  gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  char network_id[network_id_array->len + 1];
  memcpy (network_id, network_id_array->data, network_id_array->len);
  network_id[network_id_array->len] = 0;

  const char *scan_status_to_str (int scan_status)
  {
    switch (scan_status)
      {
      case ICD_SCAN_NEW: return "new";
      case ICD_SCAN_UPDATE: return "update";
      case ICD_SCAN_NOTIFY: return "notify";
      case ICD_SCAN_EXPIRE: return "expire";
      case ICD_SCAN_COMPLETE: return "complete";
      default: return "unknown status";
      }
  }

  uint64_t last_seen_delta = now () - 1000 * (uint64_t) last_seen;
  debug (4, DEBUG_BOLD ("Status: %s")"; last seen: "TIME_FMT"; "
	 "Service: %s; %s; %x; %s; %d; "
	 "Network: %s; %s; %x; %s; %d; "
	 "signal: %d (%d dB); station: %s",
	 scan_status_to_str (status), TIME_PRINTF (last_seen_delta),
	 service_type, service_name, service_attributes,
	 service_id, service_priority,
	 network_type, network_name, network_attributes,
	 network_id, network_priority,
	 signal_strength, signal_strength_db, station_id);

  if (! m->network_type_to_scan_results_hash)
    m->network_type_to_scan_results_hash
      = g_hash_table_new (g_str_hash, g_str_equal);

  GSList *results = g_hash_table_lookup (m->network_type_to_scan_results_hash,
					 network_type);
  int orig_len = 0;
  if (results)
    /* Skip the key.  */
    {
      assert (strcmp (results->data, network_type) == 0);
      results = results->next;
      assert (results);
      orig_len = g_slist_length (results);
    }
  
  void results_set (const char *network_type, GSList *results)
  {
    /* Only the first element of the list (the key) is guaranteed to
       be valid; it's next pointer may now be invalid.  */
    GSList *orig = g_hash_table_lookup (m->network_type_to_scan_results_hash,
					network_type);
    debug (5, "Updating %s: %d -> %d results (%p; %p)",
	   network_type, orig_len,
	   results ? g_slist_length (results) : 0,
	   orig, results);

    if (orig && results)
      orig->next = results;
    else if (orig)
      /* Results is now empty.  */
      {
	g_hash_table_remove (m->network_type_to_scan_results_hash,
			     network_type);
	gpointer t = g_hash_table_lookup (m->network_type_to_scan_results_hash,
					  network_type);
	if (t)
	  debug (0, "orig: %p; after remove: %p", orig, t);
	assert (! t);

	g_free (orig->data);
	g_slist_free_1 (orig);
      }
    else if (results)
      /* Now have something.  */
      {
	char *key = g_strdup (network_type);
	results = g_slist_prepend (results, key);
	g_hash_table_insert (m->network_type_to_scan_results_hash,
			     key, results);
      }
  }
  
  if (status == ICD_SCAN_COMPLETE)
    /* Scan complete.  Send the results to the user.  */
    {
      debug (4, "Scan for %s completed.", network_type);
      g_signal_emit (m,
		     NC_NETWORK_MONITOR_GET_CLASS
		     (m)->scan_results_signal_id, 0, results);

      debug (5, "Freeing %d results for %s",
	     g_slist_length (results), network_type);

      GSList *n = results;
      while (n)
	{
	  results = n;
	  n = results->next;
	  g_free (results->data);
	  g_slist_free_1 (results);
	}

      results_set (network_type, NULL);

      if (m->am_scanning)
	{
	  m->am_scanning --;
	  if (m->am_scanning == 0)
	    {
	      GError *error = NULL;
	      if (! com_nokia_icd2_scan_cancel_req (m->icd2_proxy, &error))
		{
		  debug (0, "Error invoking scan_cancel_req: %s",
			 error->message);
		  g_error_free (error);
		}
	    }
	}

      return;
    }

  struct nm_ap *ap = g_malloc (sizeof *ap + strlen (network_id) + 1
			       + strlen (network_name) + 1
			       + strlen (station_id) + 1
			       + strlen (network_type) + 1);

  char *p = (char *) &ap[1];
  ap->network_id = p;
  p = mempcpy (p, network_id, strlen (network_id) + 1);

  ap->user_id = p;
  p = mempcpy (p, network_name, strlen (network_name) + 1);

  ap->station_id = p;
  p = mempcpy (p, station_id, strlen (station_id) + 1);

  ap->network_type = p;
  p = mempcpy (p, network_type, strlen (network_type) + 1);

  ap->network_flags = network_attributes;
  ap->signal_strength_db = signal_strength_db;
  ap->signal_strength_normalized = signal_strength;

  results = g_slist_prepend (results, ap);
  results_set (network_type, results);
}

void
nm_scan (NCNetworkMonitor *m)
{
  GError *error = NULL;
  char **networks_to_scan = NULL;
  if (! com_nokia_icd2_scan_req (m->icd2_proxy,
				 ICD_SCAN_REQUEST_ACTIVE, &networks_to_scan,
				 &error))
    {
      debug (0, "Error invoking scan_req: %s", error->message);
      g_error_free (error);
    }
  else
    {
      int i;
      for (i = 0; networks_to_scan[i]; i ++)
	{
	  debug (4, "Scanning %s", networks_to_scan[i]);
	  g_free (networks_to_scan[i]);
	}
      g_free (networks_to_scan);

      m->am_scanning = i;
    }
}

/* Enumerate all network devices, create corresponding local objects
   and start listening for state changes.  */
static gboolean
start (gpointer user_data)
{
  NCNetworkMonitor *m = NC_NETWORK_MONITOR (user_data);

  /* ADDRINFO_REQ causes ICD2 to enumerate all of the connection's and
     their status.  */
  unsigned int count = 0;
  GError *error = NULL;
  if (com_nokia_icd2_addrinfo_req (m->icd2_proxy, &count, &error))
    debug (4, DEBUG_BOLD ("%d active connections"), count);
  else
    {
      debug (0, "Error invoking addrinfo_req: %s", error->message);
      g_error_free (error);
    }

  /* Don't run this idle handler again.  */
  return false;
}

static void
nc_network_monitor_backend_init (NCNetworkMonitor *m)
{
  m->icd2_proxy = dbus_g_proxy_new_for_name
    (m->system_bus,
     ICD_DBUS_API_INTERFACE,
     ICD_DBUS_API_PATH,
     ICD_DBUS_API_INTERFACE);

  /* addrinfo_sig.  */
  assssss = dbus_g_type_get_collection
    ("GPtrArray",
     dbus_g_type_get_struct ("GValueArray",
			     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
			     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
			     G_TYPE_INVALID));

  dbus_g_object_register_marshaller
    (g_cclosure_user_marshal_VOID__STRING_UINT_STRING_STRING_UINT_BOXED_BOXED,
     G_TYPE_NONE,
     G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
     G_TYPE_STRING, G_TYPE_UINT, DBUS_TYPE_G_UCHAR_ARRAY, assssss,
     G_TYPE_INVALID);

  dbus_g_proxy_add_signal (m->icd2_proxy, ICD_DBUS_API_ADDRINFO_SIG,
			   G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
			   G_TYPE_STRING, G_TYPE_UINT, DBUS_TYPE_G_UCHAR_ARRAY,
			   assssss,
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (m->icd2_proxy, ICD_DBUS_API_ADDRINFO_SIG,
			       G_CALLBACK (addrinfo_sig_cb),
			       m, NULL);

  /* state_sig.  */
  dbus_g_object_register_marshaller
    (g_cclosure_user_marshal_VOID__STRING_UINT_STRING_STRING_UINT_BOXED_STRING_UINT,
     G_TYPE_NONE,
     G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
     G_TYPE_STRING, G_TYPE_UINT, DBUS_TYPE_G_UCHAR_ARRAY,
     G_TYPE_STRING, G_TYPE_UINT,
     G_TYPE_INVALID);

  dbus_g_proxy_add_signal (m->icd2_proxy, ICD_DBUS_API_STATE_SIG,
			   G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
			   G_TYPE_STRING, G_TYPE_UINT, DBUS_TYPE_G_UCHAR_ARRAY,
			   G_TYPE_STRING, G_TYPE_UINT,
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (m->icd2_proxy, ICD_DBUS_API_STATE_SIG,
			       G_CALLBACK (icd2_state_sig_cb),
			       m, NULL);

  /* scan_sig.  */
  dbus_g_object_register_marshaller
    (g_cclosure_user_marshal_VOID__UINT_UINT_STRING_STRING_UINT_STRING_INT_STRING_STRING_UINT_BOXED_INT_INT_STRING_INT,
     G_TYPE_NONE,
     G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING,
     G_TYPE_UINT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
     G_TYPE_STRING, G_TYPE_UINT, DBUS_TYPE_G_UCHAR_ARRAY,
     G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_INT,
     G_TYPE_INVALID);

  dbus_g_proxy_add_signal (m->icd2_proxy, ICD_DBUS_API_SCAN_SIG,
			   G_TYPE_UINT, G_TYPE_UINT,
			   G_TYPE_STRING, G_TYPE_STRING,
			   G_TYPE_UINT, G_TYPE_STRING,
			   G_TYPE_INT, G_TYPE_STRING,
			   G_TYPE_STRING, G_TYPE_UINT,
			   DBUS_TYPE_G_UCHAR_ARRAY,
			   G_TYPE_INT, G_TYPE_INT,
			   G_TYPE_STRING, G_TYPE_INT,
			   G_TYPE_INVALID);

  dbus_g_proxy_connect_signal (m->icd2_proxy, ICD_DBUS_API_SCAN_SIG,
			       G_CALLBACK (icd2_scan_sig_cb),
			       m, NULL);

  /* Query devices and extant connections the next time things are
     idle.  */
  g_idle_add (start, m);
}
