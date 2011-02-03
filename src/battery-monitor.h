/* battery-monitor.h - Battery monitor interface.
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

#ifndef WOODCHUCK_BATTERY_MONITOR_H
#define WOODCHUCK_BATTERY_MONITOR_H

#include <glib-object.h>

#include <stdint.h>

#include "config.h"

/* Battery monitor's interface.  */
typedef struct _WCBatteryMonitor WCBatteryMonitor;
typedef struct _WCBatteryMonitorClass WCBatteryMonitorClass;

#define WC_BATTERY_MONITOR_TYPE (wc_battery_monitor_get_type ())
#define WC_BATTERY_MONITOR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), WC_BATTERY_MONITOR_TYPE, WCBatteryMonitor))
#define WC_BATTERY_MONITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), WC_BATTERY_MONITOR_TYPE, WCBatteryMonitorClass))
#define IS_WC_BATTERY_MONITOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), WC_BATTERY_MONITOR_TYPE))
#define IS_WC_BATTERY_MONITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), WC_BATTERY_MONITOR_TYPE))
#define WC_BATTERY_MONITOR_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), WC_BATTERY_MONITOR_TYPE, WCBatteryMonitorClass))

enum
  {
    WC_BATTERY_CHARGER_UNKNOWN = 0,
    WC_BATTERY_CHARGER_NONE = 1,
    WC_BATTERY_CHARGER_WALL = 2,
    WC_BATTERY_CHARGER_USB = 3
  };

static inline const char *
wc_battery_charger_to_string (int charger)
{
  switch (charger)
    {
    default:
      return "invalid";
    case WC_BATTERY_CHARGER_UNKNOWN:
      return "unknown";
    case WC_BATTERY_CHARGER_NONE:
      return "none";
    case WC_BATTERY_CHARGER_WALL:
      return "wall";
    case WC_BATTERY_CHARGER_USB:
      return "usb";
    }
}

struct _WCBatteryMonitorClass
{
  GObjectClass parent;

  /* "battery-status" signal: Called when a battery's status changes.
     Takes 11 parameters.  The first is the battery object whose
     status changed.  The remainder are integers.  The first two are
     the old and new value of is-charging, etc.

       - is-charging: whether the device is charging (0/1)
       - is-discharging: whether the device is discharging (0/1)
       - mV: current Voltage in milli-volts
       - mAh: remaining energy, in milli-Amper Hours
       - charger: the type of the connected charger, if any

     If any of the values are unknown they are passed as -1.
  */
  guint battery_status_signal_id;
};

extern GType wc_battery_monitor_get_type (void);

/* Battery's interface.  */
typedef struct _WCBattery WCBattery;
typedef struct _WCBatteryClass WCBatteryClass;

#define WC_BATTERY_TYPE (wc_battery_get_type ())
#define WC_BATTERY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), WC_BATTERY_TYPE, WCBattery))
#define WC_BATTERY_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), WC_BATTERY_TYPE, WCBatteryClass))
#define IS_WC_BATTERY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), WC_BATTERY_TYPE))
#define IS_WC_BATTERY_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), WC_BATTERY_TYPE))
#define WC_BATTERY_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), WC_BATTERY_TYPE, WCBatteryClass))

struct _WCBatteryClass
{
  GObjectClass parent;
};

extern GType wc_battery_get_type (void);

/* Instantiate a idle monitor.  After instantiating, immediately
   connect to the object's "idle" and "disconnected" signals: existing
   connections will be created at the next "idle" point.  */
extern WCBatteryMonitor *wc_battery_monitor_new (void);

/* List the currently plugged-in batteries.  Returns a list of
   WCBattery objects.  The caller must free the returned list and
   unref the objects:

     g_slist_foreach (list, (GFunc) g_object_unref, NULL);
     g_slist_free (list);
*/
extern GSList *wc_battery_monitor_list (WCBatteryMonitor *m);

/* Returns a battery's identifier.  */
extern const char *wc_battery_id (WCBattery *b);

/* Return the battery's design voltage and ampere-hours.  Returns -1
   if unknown.  */
extern int wc_battery_mv_design (WCBattery *b);
extern int wc_battery_mah_design (WCBattery *b);

/* Return recent values (=> a refresh is forced if the values are
   older than a few seconds) of different properties.  */
extern int wc_battery_is_charging (WCBattery *b);
extern int wc_battery_is_discharging (WCBattery *b);
extern int wc_battery_mv (WCBattery *b);
extern int wc_battery_mah (WCBattery *b);
extern int wc_battery_charger (WCBattery *b);

/* Force the battery's properties to be refresh.  If any properties
   have changed, a signal is sent and true is returned.  */
extern gboolean wc_battery_refresh_properties (WCBattery *b);

#endif
