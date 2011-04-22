/* battery-monitor.c - Battery monitor.
   Copyright 2011 Neal H. Walfield <neal@walfield.org>

   This file is part of Woodchuck.

   Woodchuck is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   Woodchuck is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "battery-monitor.h"

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

#include "org.freedesktop.Hal.Manager.h"
#include "org.freedesktop.Hal.Device.h"

#include "marshal.h"

#include "dbus-util.h"
#include "util.h"
#include "debug.h"

struct _WCBattery
{
  GObject parent;

  /* For HAL: the battery's d-bus path.  */
  char *name;

  /* Proxy object for the device.
     Service name: org.freedesktop.Hal
     Object path: name
     Interface: org.freedesktop.Hal.Device  */
  DBusGProxy *hal_device_proxy;

  /* Source id of any extent properties_reread callback.  */
  guint properties_reread;

  /* Back pointer.  */
  WCBatteryMonitor *monitor;

  /* The last time we updated the values.  */
  uint64_t last_update;
  /* Each time a property changes, this is incremented.  */
  int version;

  /* Whether the device is charging/discharging.  */
  int is_charging;
  int is_discharging;
  /* The last read mV and mah.  */
  int mv;
  int mah;
  /* The type of attached charger (if any).  */
  int charger;
};

struct _WCBatteryMonitor
{
  GObject parent;

  /* System bus.  */
  DBusGConnection *system_bus;

  /* Proxy object for the HAL manager.
     Service name: org.freedesktop.Hal
     Object path: /org/freedesktop/Hal/Manager
     Interface: org.freedesktop.Hal.Manager  */
  DBusGProxy *hal_manager_proxy;

  /* A list of WCBattery objects.  */
  GList *batteries;
};

static gboolean
hal_device_get_property_bool (WCBattery *b, const char *prop, gboolean *value)
{
  GError *error = NULL;
  if (! (org_freedesktop_Hal_Device_get_property_boolean
	 (b->hal_device_proxy, prop, value, NULL)))
    {
      debug (0, "Error getting %s property: %s", prop, error->message);
      return FALSE;
    }

  return TRUE;
}

static gboolean
hal_device_get_property_int (WCBattery *b, const char *prop, gint *value)
{
  GError *error = NULL;
  if (! (org_freedesktop_Hal_Device_get_property_integer
	 (b->hal_device_proxy, prop, value, NULL)))
    {
      debug (0, "Error getting %s property: %s", prop, error->message);
      return FALSE;
    }

  return TRUE;
}

static gboolean
hal_device_get_property_string (WCBattery *b, const char *prop, char **value)
{
  GError *error = NULL;
  if (! (org_freedesktop_Hal_Device_get_property_string
	 (b->hal_device_proxy, prop, value, NULL)))
    {
      debug (0, "Error getting %s property: %s", prop, error->message);
      return FALSE;
    }

  return TRUE;
}

/* As battery updates come in waves, we defer rereading the properties
   for a second.  This it the corresponding callback.  */
static gboolean
battery_properties_reread (gpointer user_data)
{
  WCBattery *b = WC_BATTERY (user_data);
  b->properties_reread = 0;

  debug (5, "Rereading properties...");

  int old_is_charging = b->is_charging;
  int old_is_discharging = b->is_discharging;
  int old_mv = b->mv;
  int old_mah = b->mah;
  int old_charger = b->charger;

  int t;

  if (hal_device_get_property_bool (b, "battery.rechargeable.is_charging", &t))
    b->is_charging = t;

  if (hal_device_get_property_bool (b, "battery.rechargeable.is_discharging",
				    &t))
    b->is_discharging = t;

  if (hal_device_get_property_int (b, "battery.voltage.current", &t))
    b->mv = t;

  if (hal_device_get_property_int (b, "battery.reporting.current", &t))
    b->mah = t;

  char *c = NULL;
  hal_device_get_property_string (b, "maemo.charger.type", &c);
  if (! c)
    b->charger = WC_BATTERY_CHARGER_UNKNOWN;
  else if (strcmp (c, "none") == 0)
    b->charger = WC_BATTERY_CHARGER_NONE;
  else if (strcmp (c, "wall charger") == 0)
    b->charger = WC_BATTERY_CHARGER_WALL;
  else if (strcmp (c, "host 500 mA") == 0)
    b->charger = WC_BATTERY_CHARGER_USB;
  g_free (c);


  b->last_update = now ();

  if (old_is_charging != b->is_charging
      || old_is_discharging != b->is_discharging
      || old_mv != b->mv
      || old_mah != b->mah
      || old_charger != b->charger)
    /* Something actually changed...  */
    {
      b->version ++;

      debug (4, "Something changed: "
	     "charging: %d -> %d; discharging: %d -> %d; "
	     "mv: %d -> %d; mah: %d -> %d; charger: %s -> %s",
	     old_is_charging, b->is_charging,
	     old_is_discharging, b->is_discharging,
	     old_mv, b->mv, old_mah, b->mah,
	     wc_battery_charger_to_string (old_charger),
	     wc_battery_charger_to_string (b->charger));

      g_signal_emit (b->monitor,
		     WC_BATTERY_MONITOR_GET_CLASS (b->monitor)
		       ->battery_status_signal_id,
		     0, 
		     b, old_is_charging, b->is_charging,
		     old_is_discharging, b->is_discharging,
		     old_mv, b->mv, old_mah, b->mah,
		     old_charger, b->charger);
    }
  else
    debug (4, "Gratutitious status update:  nothing changed.");

  /* Don't run this idle handler again.  */
  return FALSE;
}

/* A battery's properties were modified.  */
static void
battery_properties_modified_cb (DBusGProxy *proxy,
				guint count,
				GPtrArray *properties,
				gpointer user_data)
{
  WCBattery *b = WC_BATTERY (user_data);

  const char *interesting_properties[]
    = { "battery.rechargeable.is_discharging",
	"battery.rechargeable.is_charging",
	"battery.voltage.current",
	"battery.reporting.current",
	"maemo.charger.type"
  };

  int i;
  for (i = 0;
       (b->properties_reread == 0 || output_debug >= 2) && i < properties->len;
       i ++)
    {
      GValueArray *strct = g_ptr_array_index (properties, i);

      const char *prop = g_value_get_string (g_value_array_get_nth (strct, 0));
      // bool added = g_value_get_boolean (g_value_array_get_nth (strct, 1));
      // bool removed = g_value_get_boolean (g_value_array_get_nth (strct, 2));

      int j;
      for (j = 0;
	   j < (sizeof (interesting_properties)
		/ sizeof (interesting_properties[0]));
	   j ++)
	if (strcmp (prop, interesting_properties[j]) == 0)
	  {
	    debug (4, "%s changed.  Queuing battery reread.", prop);

	    if (! b->properties_reread)
	      /* Don't use an idle handler.  Allow changes to
		 accumulate to reduce d-bus traffic.  */
	      b->properties_reread
		= g_timeout_add (1000, battery_properties_reread, b);

	    break;
	  }

      if (j == (sizeof (interesting_properties)
		/ sizeof (interesting_properties[0])))
	debug (4, "%s changed, but we don't care.", prop);
    }
}

const char *
wc_battery_id (WCBattery *b)
{
  return b->name;
}

gboolean
wc_battery_refresh_properties (WCBattery *b)
{
  int version = b->version;
  battery_properties_reread (b);
  return b->version != version;
}

/* Before returning a property to the user, make sure the values are
   relatively fresh.  */
static void
battery_check_freshness (WCBattery *b)
{
  if (b->last_update + 5000 < now ())
    wc_battery_refresh_properties (b);
}

int
wc_battery_is_charging (WCBattery *b)
{
  battery_check_freshness (b);
  return b->is_charging;
}

int
wc_battery_is_discharging (WCBattery *b)
{
  battery_check_freshness (b);
  return b->is_discharging;
}

int
wc_battery_mv (WCBattery *b)
{
  battery_check_freshness (b);
  return b->mv;
}

int
wc_battery_mah (WCBattery *b)
{
  battery_check_freshness (b);
  return b->mah;
}

int
wc_battery_charger (WCBattery *b)
{
  battery_check_freshness (b);
  return b->charger;
}

int
wc_battery_mv_design (WCBattery *b)
{
  int v = -1;
  hal_device_get_property_int (b, "battery.voltage.design", &v);
  return v;
}

int
wc_battery_mah_design (WCBattery *b)
{
  int v = -1;
  hal_device_get_property_int (b, "battery.reporting.design", &v);
  return v;
}

static WCBattery *
wc_battery_new (WCBatteryMonitor *m, const char *name)
{
  WCBattery *b = WC_BATTERY (g_object_new (WC_BATTERY_TYPE, NULL));

  debug (4, "Adding battery %s", name);

  b->monitor = m;

  b->name = g_strdup (name);

  m->batteries = g_list_prepend (m->batteries, b);

  b->hal_device_proxy = dbus_g_proxy_new_from_proxy
    (m->hal_manager_proxy, "org.freedesktop.Hal.Device", name);

  /* property modified signal.  */
  static GType asbb;
  if (! asbb)
    {
      asbb = dbus_g_type_get_collection
	("GPtrArray",
	 dbus_g_type_get_struct ("GValueArray",
				 G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
				 G_TYPE_INVALID));

      dbus_g_object_register_marshaller
	(g_cclosure_user_marshal_VOID__INT_BOXED,
	 G_TYPE_NONE, G_TYPE_INT, asbb, G_TYPE_INVALID);
    }

  dbus_g_proxy_add_signal (b->hal_device_proxy, "PropertyModified",
			   G_TYPE_INT, asbb, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (b->hal_device_proxy, "PropertyModified",
			       G_CALLBACK (battery_properties_modified_cb),
			       b, NULL);

  return b;
}

static void wc_battery_dispose (GObject *object);

G_DEFINE_TYPE (WCBattery, wc_battery, G_TYPE_OBJECT);

static void
wc_battery_class_init (WCBatteryClass *klass)
{
  wc_battery_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = wc_battery_dispose;

  // WCBatteryClass *bm_class = WC_BATTERY_CLASS (klass);
}

static void
wc_battery_init (WCBattery *b)
{
  b->is_charging = -1;
  b->is_discharging = -1;
  b->mv = -1;
  b->mah = -1;
}

static void
wc_battery_dispose (GObject *object)
{
  WCBattery *b = WC_BATTERY (object);
  debug (4, "Disposing battery object %s", b->name);

  WCBatteryMonitor *m = b->monitor;

  if (b->properties_reread)
    {
      g_source_remove (b->properties_reread);
      b->properties_reread = 0;
    }

  if (b->hal_device_proxy)
    {
      b->hal_device_proxy = NULL;
      g_object_unref (b->hal_device_proxy);
    }

  m->batteries = g_list_remove (m->batteries, b);

  g_free (b->name);
  b->name = NULL;
}

/* The implementation of the user activity monitor object.  */

GSList *
wc_battery_monitor_list (WCBatteryMonitor *m)
{
  GSList *list = NULL;
  GList *l;
  for (l = m->batteries; l; l = l->next)
    {
      WCBattery *b = WC_BATTERY (l->data);
      g_object_ref (b);
      list = g_slist_prepend (list, b);
    }
  
  return list;
}

WCBatteryMonitor *
wc_battery_monitor_new (void)
{
  /* The battery monitor service is a singleton.  */
  static WCBatteryMonitor *wc_battery_monitor;

  if (wc_battery_monitor)
    g_object_ref (wc_battery_monitor);
  else
    wc_battery_monitor
      = WC_BATTERY_MONITOR (g_object_new (WC_BATTERY_MONITOR_TYPE, NULL));

  return wc_battery_monitor;
}

static void wc_battery_monitor_dispose (GObject *object);

G_DEFINE_TYPE (WCBatteryMonitor, wc_battery_monitor, G_TYPE_OBJECT);

static void
wc_battery_monitor_class_init (WCBatteryMonitorClass *klass)
{
  wc_battery_monitor_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = wc_battery_monitor_dispose;

  WCBatteryMonitorClass *bm_class = WC_BATTERY_MONITOR_CLASS (klass);
  bm_class->battery_status_signal_id
    = g_signal_new ("battery-status",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_user_marshal_VOID__POINTER_INT_INT_INT_INT_INT_INT_INT_INT_INT_INT,
		    G_TYPE_NONE, 11,
		    G_TYPE_POINTER,
		    G_TYPE_INT, G_TYPE_INT,
		    G_TYPE_INT, G_TYPE_INT,
		    G_TYPE_INT, G_TYPE_INT,
		    G_TYPE_INT, G_TYPE_INT,
		    G_TYPE_INT, G_TYPE_INT);
}

static void
wc_battery_monitor_init (WCBatteryMonitor *m)
{
  GError *error = NULL;
  m->system_bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
  if (error)
    {
      g_critical ("Getting system bus: %s", error->message);
      g_error_free (error);
      return;
    }

  m->hal_manager_proxy = dbus_g_proxy_new_for_name
    (m->system_bus,
     "org.freedesktop.Hal",
     "/org/freedesktop/Hal/Manager",
     "org.freedesktop.Hal.Manager");

  char **devices = NULL;
  if (! org_freedesktop_Hal_Manager_find_device_by_capability
      (m->hal_manager_proxy, "battery", &devices, &error))
    {
      debug (0, "Failed to list batteries: %s", error->message);
      g_error_free (error);
      return;
    }

  int i;
  for (i = 0; devices[i]; i ++)
    {
      wc_battery_new (m, devices[i]);
      g_free (devices[i]);
    }
  g_free (devices);
  debug (4, "Found %d batteries.", i);
}

static void
wc_battery_monitor_dispose (GObject *object)
{
  g_critical ("Attempt to dispose battery monitor singleton!");
}
