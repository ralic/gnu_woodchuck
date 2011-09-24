/* murmeltier.c - A network and storage manager implementation.
   Copyright (C) 2011 Neal H. Walfield <neal@walfield.org>

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
#include "network-monitor.h"
#include "user-activity-monitor.h"

#include <assert.h>
#include <stdio.h>
#include <error.h>
#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>
#include <sqlite3.h>

#include "murmeltier-dbus-server.h"

#include "org.woodchuck.upcall.h"

#include "debug.h"
#include "util.h"
#include "dotdir.h"

#define G_MURMELTIER_ERROR murmeltier_error_quark ()
static GQuark
murmeltier_error_quark (void)
{
  return g_quark_from_static_string ("murmeltier");
}

struct subscription
{
  char *manager;
  gboolean descendents_too;
  char *dbus_name;
  DBusGProxy *proxy;
  char *handle;
  char data[];
};

typedef struct _Murmeltier Murmeltier;
typedef struct _MurmeltierClass MurmeltierClass;

#define MURMELTIER_TYPE (murmeltier_get_type ())
#define MURMELTIER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), MURMELTIER_TYPE, Murmeltier))
#define MURMELTIER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), MURMELTIER_TYPE, MurmeltierClass))
#define IS_MURMELTIER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MURMELTIER_TYPE))
#define IS_MURMELTIER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), MURMELTIER_TYPE))
#define MURMELTIER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), MURMELTIER_TYPE, MurmeltierClass))

struct _MurmeltierClass
{
  GObjectClass parent;
};

struct _Murmeltier
{
  GObject parent;

  DBusGProxy *dbus_proxy;

  NCNetworkMonitor *nm;
  WCUserActivityMonitor *uam;
  guint user_really_idling_timeout_id;

  DBusGConnection *session_bus;

  sqlite3 *connectivity_db;

  /* Subscription management variables.  */

  /* A hash from a subscription handle to a struct subscription *.  */
  GHashTable *handle_to_subscription_hash;
  /* A hash from a manager's UUID to a GSList * of struct subscription
     *.  */
  GHashTable *manager_to_subscription_list_hash;
  /* A hash from a client's bus name to a GSList * of struct
     subscription *.  */
  GHashTable *bus_name_to_subscription_list_hash;
};

/* There is a single instance.  */
static Murmeltier *mt;
static sqlite3 *db = NULL;

extern GType murmeltier_get_type (void);

struct property
{
  char *name;
  GType type;
  bool readwrite;
};

static struct property manager_properties[]
  = { { "HumanReadableName", G_TYPE_STRING, true },
      { "DBusServiceName", G_TYPE_STRING, true },
      { "DBusObject", G_TYPE_STRING, true },
      { "Cookie", G_TYPE_STRING, true },
      { "Priority", G_TYPE_UINT, true },
      { "DiscoveryTime", G_TYPE_UINT64, true },
      { "PublicationTime", G_TYPE_UINT64, true },
      /* Readonly.  */
      { "RegistrationTime", G_TYPE_UINT64, false },
      { "ParentUUID", G_TYPE_STRING, false },
      { NULL, G_TYPE_INVALID, false }
};

static struct property stream_properties[]
  = { { "HumanReadableName", G_TYPE_STRING, true },
      { "Cookie", G_TYPE_STRING, true },
      { "Priority", G_TYPE_UINT, true },
      { "Freshness", G_TYPE_UINT, true },
      { "ObjectsMostlyInline", G_TYPE_BOOLEAN, true },
      /* Readonly.  */
      { "RegistrationTime", G_TYPE_UINT64, false },
      { "ParentUUID", G_TYPE_STRING, false },
      { NULL, G_TYPE_INVALID, false }
};

static struct property object_properties[]
  = { { "HumanReadableName", G_TYPE_STRING, true },
      { "Cookie", G_TYPE_STRING, true },
      /* This is filled in in properties_init.  */
      { "Versions", G_TYPE_INVALID, true },
      { "Filename", G_TYPE_STRING, true },
      { "Wakeup", G_TYPE_BOOLEAN, true },
      { "TriggerTarget", G_TYPE_UINT64, true },
      { "TriggerEarliest", G_TYPE_UINT64, true },
      { "TriggerLatest", G_TYPE_UINT64, true },
      { "TransferFrequency", G_TYPE_UINT, true },
      { "DontTransfer", G_TYPE_BOOLEAN, true },
      { "NeedUpdate", G_TYPE_BOOLEAN, true },
      { "Priority", G_TYPE_UINT, true },
      { "DiscoveryTime", G_TYPE_UINT64, true },
      { "PublicationTime", G_TYPE_UINT64, true },
      /* Readonly.  */
      { "RegistrationTime", G_TYPE_UINT64, false },
      { "ParentUUID", G_TYPE_STRING, false },
      { "Instance", G_TYPE_UINT, false },
      { NULL, G_TYPE_INVALID, true },
};

static void
properties_init (void)
{
  /* XXX: Correctly initialize object_properties[Versions].  */
}

/* To avoid blocking the main loop, we send upcalls one at a time in
   an idle callback.  The state for each upcall is saved in this
   upcall data structure.  */
struct upcall
{
  int type;

  char *manager_uuid;
  char *manager_cookie;
  char *dbus_service_name;

  union
  {
#define UPCALL_STREAM_UPDATE 1
    struct
    {
      char *stream_uuid;
      char *stream_cookie;
    } stream_update;
#define UPCALL_OBJECT_TRANSFER 2
    struct
    {
      char *stream_uuid;
      char *stream_cookie;
      char *object_uuid;
      char *object_cookie;
      GValueArray *versions;
      char *filename;
      int quality;
    } object_transfer;
  };
};

static struct upcall *
upcall_stream_update (const char *dbus_service_name,
		      const char *manager_uuid,
		      const char *manager_cookie,
		      const char *stream_uuid,
		      const char *stream_cookie)
{
  int dbus_service_name_len
    = dbus_service_name ? strlen (dbus_service_name) + 1 : 0;
  int manager_uuid_len = strlen (manager_uuid) + 1;
  int manager_cookie_len = strlen (manager_cookie) + 1;
  int stream_uuid_len = strlen (stream_uuid) + 1;
  int stream_cookie_len = strlen (stream_cookie) + 1;

  struct upcall *i = g_malloc
    (sizeof (*i) + dbus_service_name_len + manager_uuid_len
     + manager_cookie_len + stream_uuid_len + stream_cookie_len);

  i->type = UPCALL_STREAM_UPDATE;

  void *p = (void *) &i[1];

  if (dbus_service_name)
    {
      i->dbus_service_name = p;
      p = mempcpy (p, dbus_service_name, dbus_service_name_len);
    }
  else
    i->dbus_service_name = NULL;

  i->manager_uuid = p;
  p = mempcpy (p, manager_uuid, manager_uuid_len);

  i->manager_cookie = p;
  p = mempcpy (p, manager_cookie, manager_cookie_len);

  i->stream_update.stream_uuid = p;
  p = mempcpy (p, stream_uuid, stream_uuid_len);

  i->stream_update.stream_cookie = p;
  p = mempcpy (p, stream_cookie, stream_cookie_len);

  return i;
}

static struct upcall *
upcall_transfer_object (const char *dbus_service_name,
			const char *manager_uuid,
			const char *manager_cookie,
			const char *stream_uuid,
			const char *stream_cookie,
			const char *object_uuid,
			const char *object_cookie,
			GValueArray *versions,
			const char *filename,
			int quality)
{
  int dbus_service_name_len
    = dbus_service_name ? strlen (dbus_service_name) + 1 : 0;
  int manager_uuid_len = strlen (manager_uuid) + 1;
  int manager_cookie_len = strlen (manager_cookie) + 1;
  int stream_uuid_len = strlen (stream_uuid) + 1;
  int stream_cookie_len = strlen (stream_cookie) + 1;
  int object_uuid_len = strlen (object_uuid) + 1;
  int object_cookie_len = strlen (object_cookie) + 1;
  int filename_len = strlen (filename) + 1;

  struct upcall *i = g_malloc
    (sizeof (*i) + dbus_service_name_len + manager_uuid_len
     + manager_cookie_len + stream_uuid_len + stream_cookie_len
     + object_uuid_len + object_cookie_len + filename_len);

  i->type = UPCALL_OBJECT_TRANSFER;

  void *p = (void *) &i[1];

  if (dbus_service_name)
    {
      i->dbus_service_name = p;
      p = mempcpy (p, dbus_service_name, dbus_service_name_len);
    }
  else
    i->dbus_service_name = NULL;

  i->manager_uuid = p;
  p = mempcpy (p, manager_uuid, manager_uuid_len);

  i->manager_cookie = p;
  p = mempcpy (p, manager_cookie, manager_cookie_len);

  i->object_transfer.stream_uuid = p;
  p = mempcpy (p, stream_uuid, stream_uuid_len);

  i->object_transfer.stream_cookie = p;
  p = mempcpy (p, stream_cookie, stream_cookie_len);

  i->object_transfer.object_uuid = p;
  p = mempcpy (p, object_uuid, object_uuid_len);

  i->object_transfer.object_cookie = p;
  p = mempcpy (p, object_cookie, object_cookie_len);

  i->object_transfer.filename = p;
  p = mempcpy (p, filename, filename_len);

  i->object_transfer.versions = versions;
  i->object_transfer.quality = quality;

  return i;
}


static void
upcall_execute (struct upcall *i)
{
  assertx (i->type == UPCALL_STREAM_UPDATE
	   || i->type == UPCALL_OBJECT_TRANSFER,
	   "type: %d", i->type);

  void do_upcall (DBusGProxy *proxy, const char *handle)
  {
    GError *error = NULL;

    if (i->type == UPCALL_STREAM_UPDATE)
      {
	debug (4, "Executing org_woodchuck_upcall_stream_update "
	       "(%s, %s, %s, %s, %s)",
	       handle,
	       i->manager_uuid,
	       i->manager_cookie,
	       i->stream_update.stream_uuid,
	       i->stream_update.stream_cookie);

	if (! (org_woodchuck_upcall_stream_update
	       (proxy,
		i->manager_uuid,
		i->manager_cookie,
		i->stream_update.stream_uuid,
		i->stream_update.stream_cookie,
		&error)))
	  {
	    debug (0, "Executing org_woodchuck_upcall_stream_update "
		   "(%s, %s, %s, %s, %s) upcall failed: %s",
		   handle,
		   i->manager_uuid,
		   i->manager_cookie,
		   i->stream_update.stream_uuid,
		   i->stream_update.stream_cookie,
		   error ? error->message : "<Unknown>");

	    if (error)
	      g_error_free (error);
	    error = NULL;
	  }
      }
    else if (i->type == UPCALL_OBJECT_TRANSFER)
      {
	debug (4, "Executing org_woodchuck_upcall_object_transfer "
	       "(%s, %s, %s, %s, %s, %s, %s, [versions], %s, %d)",
	       handle,
	       i->manager_uuid,
	       i->manager_cookie,
	       i->object_transfer.stream_uuid,
	       i->object_transfer.stream_cookie,
	       i->object_transfer.object_uuid,
	       i->object_transfer.object_cookie,
	       i->object_transfer.filename,
	       i->object_transfer.quality);

	GValueArray *versions
	  = g_value_array_copy (i->object_transfer.versions);

	if (! (org_woodchuck_upcall_object_transfer
	       (proxy,
		i->manager_uuid,
		i->manager_cookie,
		i->object_transfer.stream_uuid,
		i->object_transfer.stream_cookie,
		i->object_transfer.object_uuid,
		i->object_transfer.object_cookie,
		versions,
		i->object_transfer.filename,
		i->object_transfer.quality,
		&error)))
	  {
	    debug (0, "Executing org_woodchuck_upcall_object_transfer "
		   "(%s, %s, %s, %s, %s, %s, %s, [versions], %s, %d) "
		   "upcall failed: %s",
		   handle,
		   i->manager_uuid,
		   i->manager_cookie,
		   i->object_transfer.stream_uuid,
		   i->object_transfer.stream_cookie,
		   i->object_transfer.object_uuid,
		   i->object_transfer.object_cookie,
		   i->object_transfer.filename,
		   i->object_transfer.quality,
		   error ? error->message : "<Unknown>");

	    if (error)
	      g_error_free (error);
	    error = NULL;
	  }
      }
  }

  GSList *list = g_hash_table_lookup (mt->manager_to_subscription_list_hash,
				      i->manager_uuid);

  if (! list && i->dbus_service_name && *i->dbus_service_name)
    {
      DBusGProxy *proxy = dbus_g_proxy_new_for_name
	(mt->session_bus, i->dbus_service_name,
	 "/org/woodchuck", "org.woodchuck.upcall");
      if (proxy)
	{
	  debug (3, "Starting %s", i->dbus_service_name);
	  do_upcall (proxy, "START");
	  g_object_unref (proxy);
	}
      else
	debug (0, "Failed to create a DBus proxy object for %s",
	       i->dbus_service_name);
    }
  else
    while (list)
      {
	struct subscription *s = list->data;
	list = list->next;

	do_upcall (s->proxy, s->handle);
      }

  if (i->type == UPCALL_OBJECT_TRANSFER)
    g_value_array_free (i->object_transfer.versions);
  g_free (i);
}

static GSList *upcall_list;

static gboolean
upcall_execute_callback (gpointer user_data)
{
  if (! upcall_list)
    {
      debug (3, "upcall list exhausted.");
      return FALSE;
    }

  struct upcall *i = upcall_list->data;
  upcall_list = g_slist_delete_link (upcall_list, upcall_list);

  upcall_execute (i);

  if (upcall_list)
    /* Call again.  */
    return TRUE;
  else
    /* Nothing left to do.  */
    {
      debug (3, "upcall list exhausted.");
      return FALSE;
    }
}

static guint schedule_id;

static uint64_t last_schedule;

#define IDLE_TIME_BEFORE_SCHEDULE (5 * 60)

static gboolean
do_schedule (gpointer user_data)
{
#warning Support notifications for nested managers.
  if (schedule_id)
    g_source_remove (schedule_id);
  schedule_id = 0;

  switch (wc_user_activity_monitor_status (mt->uam))
    {
    case WC_USER_ACTIVE:
      debug (3, "Not scheduling: User is active.");
      goto out;

    case WC_USER_IDLE:
      {
	int64_t idle_time = wc_user_activity_monitor_status_time (mt->uam);
	debug (3, "User idle for "TIME_FMT, TIME_PRINTF (idle_time));
	if (idle_time != -1 && idle_time < IDLE_TIME_BEFORE_SCHEDULE * 1000)
	  {
	    debug (3, "Not scheduling: User not idle long enough ("TIME_FMT").",
		   TIME_PRINTF(IDLE_TIME_BEFORE_SCHEDULE * 1000));
	    goto out;
	  }

	/* The user has been idle long enough.  Schedule.  */
	break;
      }

    default:
      /* Active/idle status is Unknown.  Ignore this criterium.  */
      break;
    }

  if (mt->user_really_idling_timeout_id)
    g_source_remove (mt->user_really_idling_timeout_id);
  mt->user_really_idling_timeout_id = 0;

  NCNetworkConnection *dc = nc_network_monitor_default_connection (mt->nm);
  if (! dc)
    /* No connection.  */
    {
      debug (3, "Not scheduling: No default connection.");
      goto out;
    }

  if ((nc_network_connection_mediums (dc)
       & ~(NC_CONNECTION_MEDIUM_ETHERNET|NC_CONNECTION_MEDIUM_WIFI)) != 0)
    /* Only use ethernet and wifi based connections.  */
    {
      char *m = 
	nc_connection_medium_to_string (nc_network_connection_mediums (dc));
      debug (3, "Not scheduling: Default connection includes components "
	     "that are neither ethernet nor Wifi (%s).",
	     m);
      g_free (m);
      goto out;
    }

  if (upcall_list)
    {
      debug (3, "Not scheduling: %d pending upcalls.",
	     g_slist_length (upcall_list));
      goto out;
    }

  /* The following is a very simple scheduler.  We look for streams
     and objects that have not been updated recently and update
     them.  */

  uint64_t n = now ();

  int streams_callback (void *cookie, int argc, char **argv, char **names)
  {
    int i = 0;
    const char *stream_uuid = argv[i] ?: ""; i ++;
    const char *stream_cookie = argv[i] ?: ""; i ++;
    const char *manager_uuid = argv[i] ?: ""; i ++;
    const char *manager_cookie = argv[i] ?: ""; i ++;
    const char *dbus_service_name = argv[i] ?: ""; i ++;
    uint32_t freshness = argv[i] ? atoi (argv[i]) : 0; i ++;
    uint64_t transfer_time = argv[i] ? atoll (argv[i]) : 0; i ++;
    uint32_t last_trys_status = argv[i] ? atoi (argv[i]) : 0; i ++;

    if (freshness == UINT32_MAX)
      /* Never update this stream.  */
      return 0;

    int64_t timeleft = 0;
    if (transfer_time)
      timeleft = (transfer_time + freshness) - n / 1000;

    debug (3, "%s: %s stream: next update in "TIME_FMT" "
	   "(transfer_time: "TIME_FMT"; freshness: "TIME_FMT")",
	   manager_cookie, stream_cookie,
	   TIME_PRINTF (1000 * timeleft),
	   TIME_PRINTF (transfer_time == 0 ? 0 : transfer_time * 1000 - n),
	   TIME_PRINTF (freshness * 1000));

    GSList *list = g_hash_table_lookup (mt->manager_to_subscription_list_hash,
					manager_uuid);

    do_debug (4)
      {
	GString *s = g_string_new ("");
	g_string_append_printf
	  (s, "Stream: %s, %s; Manager: %s, %s; "
	   "Freshness: %"PRId32"; Transfer time: %"PRId64"/delta: "TIME_FMT"; "
	   "last try's status: %"PRId32" ->",
	   stream_uuid, stream_cookie, manager_uuid, manager_cookie,
	   freshness, transfer_time,
	   TIME_PRINTF (transfer_time == 0 ? 0 : (transfer_time * 1000 - n)),
	   last_trys_status);

	if (! list)
	  g_string_append_printf (s, " NONE");
	else
	  {
	    GSList *l = list;
	    while (l)
	      {
		g_string_append_printf
		  (s, " %s", ((struct subscription *) l->data)->dbus_name);
		l = l->next;
	      }
	  }
	debug (3, "%s", s->str);
	g_string_free (s, TRUE);
      }

    if (timeleft > freshness / 4)
      /* The content is fresh enough.  */
      {
	debug (3, "%s's stream %s is fresh enough: next update in "TIME_FMT,
	       manager_cookie, stream_cookie, TIME_PRINTF (1000 * timeleft));
	return 0;
      }
    else
      debug (3, "Calling stream_update on stream %s: "
	     "timeleft ("TIME_FMT") <= "
	     "freshness ("TIME_FMT") / 4 ("TIME_FMT")",
	     stream_cookie, TIME_PRINTF (1000 * timeleft),
	     TIME_PRINTF (1000 * (uint64_t) freshness),
	     TIME_PRINTF (1000 * (uint64_t) (freshness / 4)));

    struct upcall *upcall = upcall_stream_update
      (dbus_service_name, manager_uuid, manager_cookie,
       stream_uuid, stream_cookie);

    upcall_list = g_slist_prepend (upcall_list, upcall);

    return 0;
  }
  char *errmsg = NULL;
  int err = sqlite3_exec
    (db,
     "select streams.uuid, streams.cookie,"
     "  streams.parent_uuid, managers.cookie, managers.DBusServiceName,"
     "  streams.Freshness, stream_updates.transfer_time, stream_updates.status"
     " from streams left join stream_updates"
     " on (streams.uuid == stream_updates.uuid"
     /* MAX(STREAMS_UPDATES.INSTANCE) == STREAMS.INSTANCE + 1 */
     "     and streams.instance == stream_updates.instance + 1)"
     " join managers on streams.parent_uuid == managers.uuid"
     /* A value of -1 means never update.  */
     " where streams.Freshness != (1 << 32)-1;",
     streams_callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%s", errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  int objects_callback (void *cookie, int argc, char **argv, char **names)
  {
    int i = 0;
    const char *object_uuid = argv[i] ?: ""; i ++;
    const char *object_cookie = argv[i] ?: ""; i ++;
    const char *stream_uuid = argv[i] ?: ""; i ++;
    const char *stream_cookie = argv[i] ?: ""; i ++;
    const char *manager_uuid = argv[i] ?: ""; i ++;
    const char *manager_cookie = argv[i] ?: ""; i ++;
    const char *dbus_service_name = argv[i] ?: ""; i ++;
    uint32_t transfer_frequency = argv[i] ? atoi (argv[i]) : 0; i ++;
    uint64_t transfer_time = argv[i] ? atoll (argv[i]) : 0; i ++;
    uint32_t last_trys_status = argv[i] ? atoi (argv[i]) : 0; i ++;
    uint64_t trigger_target = argv[i] ? atoll (argv[i]) : 0; i ++;
    uint64_t trigger_earliest = argv[i] ? atoll (argv[i]) : 0; i ++;
    uint64_t trigger_latest = argv[i] ? atoll (argv[i]) : 0; i ++;
    bool need_update = argv[i] ? atoi (argv[i]) : false; i ++;
    int instance = argv[i] ? atoi (argv[i]) : 0; i ++;

    debug (4, "Considering object %s(%s): transfer_time: "TIME_FMT";"
	   " last_trys_status: %"PRId32"; transfer_frequency: %"PRId32";"
	   " need update: %s",
	   object_uuid, object_cookie,
	   TIME_PRINTF (transfer_time == 0 ? 0 : transfer_time * 1000 - n),
	   last_trys_status, transfer_frequency,
	   need_update ? "true" : "false");

    if (transfer_time && last_trys_status == 0 && transfer_frequency == 0
	&& ! need_update)
      /* The object has been successfully transferred and it is a
	 one-shot object.  Ignore.  */
      {
	debug (4, "%s(%s) already transferred.",
	       object_uuid, object_cookie);
	return 0;
      }

    if (last_trys_status == 0
	&& transfer_time
	&& transfer_time + transfer_frequency / 4 * 3 > n / 1000
	&& ! need_update)
      /* The content is fresh enough.  */
      {
	debug (4, "%s(%s) Content fresh enough.",
	       object_uuid, object_cookie);
	return 0;
      }

    GSList *list = g_hash_table_lookup (mt->manager_to_subscription_list_hash,
					manager_uuid);

    do_debug (4)
      {
	GString *s = g_string_new ("");
	g_string_append_printf
	  (s, "Object: %s, %s; Stream: %s, %s; Manager: %s, %s; "
	   "transfer frequency: %"PRId32", "
	   "time: @ %"PRId64"/delta: "TIME_FMT"; "
	   "last try's status: %"PRId32"; "
	   "trigger: <%"PRId64", %"PRId64", %"PRId64">; instance: %d; "
	   "subscriptions: ",
	   object_uuid, object_cookie,
	   stream_uuid, stream_cookie, manager_uuid, manager_cookie,
	   transfer_frequency, transfer_time,
	   TIME_PRINTF (transfer_time == 0 ? 0 : (transfer_time * 1000 - n)),
	   last_trys_status, trigger_target, trigger_earliest, trigger_latest,
	   instance);

	if (! list)
	  g_string_append_printf (s, " NONE");
	else
	  {
	    GSList *l = list;
	    while (l)
	      {
		g_string_append_printf
		  (s, " %s", ((struct subscription *) l->data)->dbus_name);
		l = l->next;
	      }
	  }
	debug (3, "%s", s->str);
	g_string_free (s, TRUE);
      }

    if (! (list || (dbus_service_name && *dbus_service_name)))
      {
	debug (3, "No one ready to receive updates for "
	       "object %s(%s) in stream %s(%s) in manager %s(%s)",
	       object_uuid, object_cookie,
	       stream_uuid, stream_cookie, manager_uuid, manager_cookie);
	return 0;
      }

    /* We need to build this for each subscriber as the dbus handler
       frees the arguments.  */
    GValueArray *versions = g_value_array_new (7);

    GValue index_value = { 0 };
    g_value_init (&index_value, G_TYPE_UINT);
    g_value_set_uint (&index_value, 0);
    g_value_array_append (versions, &index_value);

#warning Get the real values from the object_versions table.
    GValue url_value = { 0 };
    g_value_init (&url_value, G_TYPE_STRING);
    g_value_set_static_string (&url_value, "");
    g_value_array_append (versions, &url_value);

    GValue expected_size_value = { 0 };
    g_value_init (&expected_size_value, G_TYPE_INT64);
    g_value_set_int64 (&expected_size_value, 0);
    g_value_array_append (versions, &expected_size_value);

    GValue expected_transfer_up_value = { 0 };
    g_value_init (&expected_transfer_up_value, G_TYPE_UINT64);
    g_value_set_uint64 (&expected_transfer_up_value, 0);
    g_value_array_append (versions, &expected_transfer_up_value);

    GValue expected_transfer_down_value = { 0 };
    g_value_init (&expected_transfer_down_value, G_TYPE_UINT64);
    g_value_set_uint64 (&expected_transfer_down_value, 0);
    g_value_array_append (versions, &expected_transfer_down_value);

    GValue utility_value = { 0 };
    g_value_init (&utility_value, G_TYPE_UINT);
    g_value_set_uint (&utility_value, 1);
    g_value_array_append (versions, &utility_value);

    GValue use_simple_transferer_value = { 0 };
    g_value_init (&use_simple_transferer_value, G_TYPE_BOOLEAN);
    g_value_set_boolean (&use_simple_transferer_value, FALSE);
    g_value_array_append (versions, &use_simple_transferer_value);

    struct upcall *upcall = upcall_transfer_object
      (dbus_service_name, manager_uuid, manager_cookie,
       stream_uuid, stream_cookie, object_uuid, object_cookie,
       versions, "", 5);

    upcall_list = g_slist_prepend (upcall_list, upcall);

    return 0;
  }
  errmsg = NULL;
  err = sqlite3_exec
    (db,
     "select objects.uuid, objects.cookie,"
     "  streams.uuid, streams.cookie,"
     "  streams.parent_uuid, managers.cookie, managers.DBusServiceName,"
     "  objects.TransferFrequency, object_instance_status.transfer_time,"
     "  object_instance_status.status,"
     "  objects.TriggerTarget, objects.TriggerEarliest, objects.TriggerLatest,"
     "  objects.NeedUpdate, objects.instance"
     " from objects left join object_instance_status"
     " on (objects.uuid == object_instance_status.uuid"
     /* MAX(OBJECT_INSTANCE_STATUS.INSTANCE) == OBJECTS.INSTANCE + 1 */
     "     and objects.Instance == object_instance_status.instance + 1)"
     " join streams on objects.parent_uuid == streams.uuid"
     " join managers on managers.uuid == streams.parent_uuid"
     " where objects.DontTransfer == 0"
     "  and (coalesce (object_instance_status.transfer_time, 0) == 0"
     "       or objects.NeedUpdate == 1"
     "       or objects.TransferFrequency > 0);",
     objects_callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%s", errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  uint64_t t = now () - n;
  debug (3, "Scheduling took "TIME_FMT, TIME_PRINTF(t));

  if (upcall_list)
    {
      debug (3, "Have %d upcalls to send", g_slist_length (upcall_list));
      upcall_list = g_slist_reverse (upcall_list);
      g_idle_add (upcall_execute_callback, NULL);
    }

 out:
  last_schedule = now ();

  /* Don't call again.  */
  return FALSE;
}

/* Call this when it appears that an action can be executed, e.g.,
   updating a stream or transferring an object.  */
static void
schedule (void)
{
  if (schedule_id)
    return;

  int64_t last_schedule_delta = (now () - last_schedule) / 1000;
  /* Minimum interval: 2 minutes.  But at least 10 sec to aggregate
     multiple events.  */
  int delay = MAX (10, 120LL - last_schedule_delta);
  debug (3, "Running scheduler in %d seconds (last schedule delta: %"PRId64")",
	 delay, last_schedule_delta);

  schedule_id = g_timeout_add_seconds (delay, do_schedule, NULL);
}

/* When a client exits, clean up any feedback subscriptions it may
   have.  */
static void
dbus_name_owner_changed_cb (DBusGProxy *proxy,
			    const char *name, const char *old_owner,
			    const char *new_owner,
			    gpointer user_data)
{
  if (! name)
    return;
  if (name[0] != ':')
    /* We are only interested in private names.  */
    return;

  GSList *list = g_hash_table_lookup (mt->bus_name_to_subscription_list_hash,
				      old_owner);
  while (list)
    /* Be careful how we traverse the list:
       woodchuck_manager_feedback_unsubscribe modifies it!  */
    {
      struct subscription *s = list->data;
      list = list->next;

      int ret = woodchuck_manager_feedback_unsubscribe (s->dbus_name,
							s->manager, s->handle,
							NULL);
      if (ret)
	debug (0, "Removing owner:%s, manager:%s, handle:%s: %s",
	       s->dbus_name, s->manager, s->handle,
	       woodchuck_error_to_error (ret));
    }
}

static gboolean
schedule_periodically (gpointer user_data)
{
  schedule ();

  /* Run again.  */
  return true;
}

/* A new connection has been established.  */
static void
new_connection (NCNetworkMonitor *nm, NCNetworkConnection *nc,
		gpointer user_data)
{
}

/* An existing connection has been brought down.  */
static void
disconnected (NCNetworkMonitor *nm, NCNetworkConnection *nc,
	      gpointer user_data)
{
}

/* There is a new default connection.  */
static void
default_connection_changed (NCNetworkMonitor *nm,
			    NCNetworkConnection *old_default,
			    NCNetworkConnection *new_default,
			    gpointer user_data)
{
  schedule ();
}

static void
user_idle_active (WCUserActivityMonitor *m,
		  int activity_status,
		  int activity_status_previous,
		  int64_t time_in_previous_state,
		  gpointer user_data)
{
  debug (4, "user became %s (was %s for "TIME_FMT")",
	 wc_user_activity_status_string (activity_status),
	 wc_user_activity_status_string (activity_status_previous),
	 TIME_PRINTF(time_in_previous_state));

  if (activity_status != WC_USER_ACTIVE)
    /* User is idle or the state is unknown.  Schedule a
       scheduling.  */
    {
      /* When the user becomes active, any pending callback is
	 cancelled.  */
      assert (! mt->user_really_idling_timeout_id);
      mt->user_really_idling_timeout_id
	= g_timeout_add_seconds (IDLE_TIME_BEFORE_SCHEDULE, do_schedule, NULL);
    }
  else
    /* The user is now active.  */
    {
      if (mt->user_really_idling_timeout_id)
	{
          g_source_remove (mt->user_really_idling_timeout_id);
	  mt->user_really_idling_timeout_id = 0;
	}
    }
}

G_DEFINE_TYPE (Murmeltier, murmeltier, G_TYPE_OBJECT);

static void
murmeltier_class_init (MurmeltierClass *klass)
{
#if 0
  murmeltier_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
#endif
}

static void
murmeltier_init (Murmeltier *mt)
{
  // MurmeltierClass *klass = MURMELTIER_GET_CLASS (mt);

  GError *error = NULL;
  mt->session_bus = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
  if (error)
    {
      g_critical ("%s: Getting system bus: %s", __FUNCTION__, error->message);
      g_error_free (error);
    }

  mt->handle_to_subscription_hash
    = g_hash_table_new (g_str_hash, g_str_equal);
  mt->manager_to_subscription_list_hash
    = g_hash_table_new (g_str_hash, g_str_equal);
  mt->bus_name_to_subscription_list_hash
    = g_hash_table_new (g_str_hash, g_str_equal);
  

  mt->dbus_proxy = dbus_g_proxy_new_for_name
    (mt->session_bus, "org.freedesktop.DBus",
     "/org/freedesktop/DBus", "org.freedesktop.DBus");

  dbus_g_proxy_add_signal (mt->dbus_proxy,
			   "NameOwnerChanged",
			   G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
			   G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (mt->dbus_proxy, "NameOwnerChanged",
			       G_CALLBACK (dbus_name_owner_changed_cb),
			       NULL, NULL);

  /* Approximately every hour, check to see if there is something that
     needs to be transferred.  */
  g_timeout_add_seconds (60 * 60, schedule_periodically, mt);


  /* Initialize the network monitor.  */
  mt->nm = nc_network_monitor_new ();

  g_signal_connect (G_OBJECT (mt->nm), "new-connection",
		    G_CALLBACK (new_connection), mt);
  g_signal_connect (G_OBJECT (mt->nm), "disconnected",
		    G_CALLBACK (disconnected), mt);
  g_signal_connect (G_OBJECT (mt->nm), "default-connection-changed",
		    G_CALLBACK (default_connection_changed), mt);

  mt->uam = wc_user_activity_monitor_new ();

  g_signal_connect (G_OBJECT (mt->uam), "user-idle-active",
		    G_CALLBACK (user_idle_active), mt);
}

static enum woodchuck_error
object_register (const char *parent, const char *parent_table,
		 const char *object_table, GHashTable *properties,
		 struct property *acceptable_properties,
		 const char *required_properties[],
		 gboolean only_if_cookie_unique,
		 char **uuid, GError **error)
{
  enum woodchuck_error ret = 0;
  gboolean abort_transaction = FALSE;

  GString *keys = g_string_new ("");
  GString *values = g_string_new ("");

  int required_properties_count = 0;
  for (; required_properties[required_properties_count];
       required_properties_count ++)
    ;

  bool have_required_property[required_properties_count];
  memset (have_required_property, 0, sizeof (have_required_property));

  char *unknown_property = NULL;
  char *bad_type = NULL;

  GPtrArray *versions = NULL;

  const char *cookie = NULL;

  debug (4, "Parent: '%s' (%p)", parent, parent);
  debug (4, "Properties:");
  void iter (gpointer keyp, gpointer valuep, gpointer user_data)
  {
    char *key = keyp;
    GValue *value = valuep;

    int i;
    for (i = 0; acceptable_properties[i].name; i ++)
      if (strcmp (key, acceptable_properties[i].name) == 0)
	break;

    if (! acceptable_properties[i].name)
      {
	unknown_property = key;
	return;
      }

    for (i = 0; required_properties[i]; i ++)
      if (strcmp (key, required_properties[i]) == 0)
	{
	  have_required_property[i] = true;
	  break;
	}

    if (! unknown_property && ! bad_type)
      {
	if (strcmp (key, "Versions") == 0)
	  {
	    static GType asxttub;
	    if (! asxttub)
	      asxttub = dbus_g_type_get_collection
		("GPtrArray",
		 dbus_g_type_get_struct ("GValueArray",
					 G_TYPE_STRING, G_TYPE_INT64,
					 G_TYPE_UINT64, G_TYPE_UINT64,
					 G_TYPE_UINT, G_TYPE_BOOLEAN,
					 G_TYPE_INVALID));

	    if (! G_VALUE_HOLDS (value, asxttub))
	      {
		debug (0, "Versions does not have type asxttub.");
		bad_type = key;
	      }
	    else if (versions)
	      {
		bad_type = key;
		debug (0, "Versions key passed multiple times.");
	      }
	    else
	      versions = g_value_get_boxed (value);
	  }
	else
	  {
	    g_string_append_printf (keys, ", %s", key);

	    if (G_VALUE_HOLDS_STRING (value))
	      {
		char *escaped
		  = sqlite3_mprintf ("%Q", g_value_get_string (value));
		g_string_append_printf (values, ", %s", escaped);
		sqlite3_free (escaped);
	      }
	    else if (G_VALUE_HOLDS_UINT (value))
	      g_string_append_printf (values, ", %d", g_value_get_uint (value));
	    else
	      bad_type = key;
	  }
      }

    if (only_if_cookie_unique && ! cookie && strcmp (key, "Cookie") == 0
	&& G_VALUE_HOLDS_STRING (value))
      cookie = g_value_get_string (value);
  }
  g_hash_table_foreach (properties, iter, NULL);

  if (unknown_property)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Unknown property: %s", unknown_property);
      ret = WOODCHUCK_ERROR_INVALID_ARGS;
      goto out;
    }
  if (bad_type)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Argument has unsupported type: %s", bad_type);
      ret = WOODCHUCK_ERROR_INVALID_ARGS;
      goto out;
    }
  GString *s = NULL;
  int i;
  for (i = 0; required_properties[i]; i ++)
    if (! have_required_property[i])
      {
	if (! s)
	  s = g_string_new ("");
	else
	  g_string_append_printf (s, ", ");
	g_string_append_printf (s, "%s", required_properties[i]);
      }
  if (s)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Missing required properties: %s", s->str);
      g_string_free (s, TRUE);

      ret = WOODCHUCK_ERROR_INVALID_ARGS;
      goto out;
    }

  if (only_if_cookie_unique)
    {
      if (! cookie)
	{
	  g_set_error (error, G_MURMELTIER_ERROR, 0,
		       "Cookie NULL not unique.");
	  ret = WOODCHUCK_ERROR_OBJECT_EXISTS;
	  goto out;
	}

      GString *uuid_other = NULL;
      int unique_callback (void *cookie, int argc, char **argv, char **names)
      {
	if (! uuid_other)
	  {
	    uuid_other = g_string_new ("");
	    g_string_append_printf (uuid_other, "%s", argv[0]);
	  }
	else
	  g_string_append_printf (uuid_other, ", %s", argv[0]);
	return 0;
      }

      char *errmsg = NULL;
      int err = sqlite3_exec_printf
	(db,
	 "select uuid from %s where cookie = '%s' and parent_uuid='%s';",
	 unique_callback, NULL, &errmsg, object_table, cookie, parent);
      if (errmsg)
	{
	  g_set_error (error, G_MURMELTIER_ERROR, 0,
		       "Internal error at %s:%d: %s",
		       __FILE__, __LINE__, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;

	  ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
	  goto out;
	}

      if (uuid_other)
	{
	  g_set_error (error, G_MURMELTIER_ERROR, 0,
		       "Cookie '%s' not unique.  Other %s with cookie: %s",
		       cookie, object_table, uuid_other->str);
	  g_string_free (uuid_other, TRUE);
	  ret = WOODCHUCK_ERROR_OBJECT_EXISTS;
	  goto out;
	}
    }

  char *errmsg = NULL;
  int err = sqlite3_exec (db, "begin transaction", NULL, NULL, &errmsg);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
      goto out;
    }
  abort_transaction = TRUE;

  *uuid = NULL;
  int uuid_callback (void *cookie, int argc, char **argv, char **names)
  {
    assert (! *uuid);
    *uuid = g_strdup (argv[0]);
    return 0;
  }

 retry:
  err = sqlite3_exec_printf
    (db,
     "insert or abort into %s"
     " (uuid, parent_uuid%s) values (lower(hex(randomblob(16))), %Q%s);"
     "select uuid from %s where ROWID = last_insert_rowid ();",
     uuid_callback, NULL, &errmsg,
     object_table, keys->str, parent ?: "", values->str, object_table);
  if (errmsg)
    {
      if (err == SQLITE_ABORT)
	/* UUID already exists.  */
	{
	  debug (0, "UUID conflict.  Trying again: %s", errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	  goto retry;
	}

      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
      goto out;
    }

  debug (0, "UUID is: %s", *uuid);

  if (versions)
    {
      GString *sql = g_string_new ("");

      int i;
      for (i = 0; i < versions->len; i ++)
	{
	  GValueArray *strct = g_ptr_array_index (versions, i);

	  const char *url
	    = g_value_get_string (g_value_array_get_nth (strct, 0));
	  char *url_escaped = sqlite3_mprintf ("%Q", url);

	  int64_t expected_size
	    = g_value_get_int64 (g_value_array_get_nth (strct, 1));
	  uint64_t expected_transfer_up
	    = g_value_get_uint64 (g_value_array_get_nth (strct, 2));
	  uint64_t expected_transfer_down
	    = g_value_get_uint64 (g_value_array_get_nth (strct, 3));
	  uint32_t utility
	    = g_value_get_uint (g_value_array_get_nth (strct, 4));
	  gboolean use_simple_transferer
	    = g_value_get_boolean (g_value_array_get_nth (strct, 5));

	  g_string_append_printf
	    (sql,
	     "insert into object_versions"
	     " (uuid, version, parent_uuid,"
	     "  url, expected_size, expected_transfer_up,"
	     "  expected_transfer_down, utility, use_simple_transferer)"
	     " values"
	     " ('%s', %d, '%s', %s, %"PRId64", %"PRIu64", %"PRIu64", %d, %d);"
	     "\n",
	     *uuid, i, parent, url_escaped, expected_size,
	     expected_transfer_up, expected_transfer_down,
	     utility, use_simple_transferer);

	  sqlite3_free (url_escaped);
	}

      sqlite3_exec_printf (db, "%s", NULL, NULL, &errmsg, sql->str);
      g_string_free (sql, TRUE);
      if (errmsg)
	{
	  g_set_error (error, G_MURMELTIER_ERROR, 0,
		       "Internal error at %s:%d: %s",
		       __FILE__, __LINE__, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;

	  ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
	  goto out;
	}
    }

  sqlite3_exec (db, "end transaction", NULL, NULL, &errmsg);
  if (errmsg)
    {
      if (! ret)
	g_set_error (error, G_MURMELTIER_ERROR, 0,
		     "Internal error at %s:%d: %s",
		     __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      if (! ret)
	ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
    }

  abort_transaction = false;

  assert (*uuid);

  schedule ();
 out:
  if (abort_transaction)
    {
      err = sqlite3_exec (db, "rollback transaction", NULL, NULL, &errmsg);
      if (errmsg)
	{
	  if (! ret)
	    g_set_error (error, G_MURMELTIER_ERROR, 0,
			 "Internal error at %s:%d: %s",
			 __FILE__, __LINE__, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;

	  if (! ret)
	    ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
	}
    }

  g_string_free (keys, TRUE);
  g_string_free (values, TRUE);

  return ret;
}

static int
list_callback (void *cookie, int argc, char **argv, char **names)
{
  GPtrArray *list = cookie;

  GPtrArray *strct = g_ptr_array_new ();

  debug (5, "Object %d: %d args", list->len, argc);

  int i;
  for (i = 0; i < argc; i ++)
    {
      if (strcmp (names[i], "parent_uuid") == 0 && argv[i][0] == 0)
	g_ptr_array_add (strct, NULL);
      else
	g_ptr_array_add (strct, g_strdup (argv[i]));

      debug (5, "Object %d: index %d: %s", list->len, i, argv[i]);
    }

  g_ptr_array_add (list, strct);

  return 0;
}

static enum woodchuck_error
lookup_by (const char *table,
	   const char *column, const char *value, const char *parent_uuid,
	   gboolean recursive,
	   const char *properties,
	   GPtrArray **objects, GError **error)
{
  *objects = g_ptr_array_new ();

  char *errmsg = NULL;
  if (! recursive)
    sqlite3_exec_printf
      (db,
       "select %s from %s where %s = %Q and parent_uuid = %Q;",
       list_callback, *objects, &errmsg,
       properties, table, column, value, parent_uuid ?: "");
  else if (! parent_uuid && recursive)
    sqlite3_exec_printf
      (db,
       "select %s from %s where %s = %Q;",
       list_callback, *objects, &errmsg,
       properties, table, column, value);
  else
#warning Implement lookup_by not recursive.
    return WOODCHUCK_ERROR_NOT_IMPLEMENTED;

  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      return WOODCHUCK_ERROR_INTERNAL_ERROR;
    }

  debug (4, "%d objects matched.", (*objects)->len);

  return 0;
}

static int
abort_if_too_many_callback (void *cookie, int argc, char **argv, char **names)
{
  int *count = cookie;

  if (*count == 0)
    return 1;

  -- *count;
  return 0;
}

static enum woodchuck_error
object_unregister (const char *uuid,
		   const char *table, const char *secondary_tables[],
		   const char *child_tables[],
		   bool only_if_no_descendents,
		   GError **error)
{
  if (only_if_no_descendents)
    {
      /* First verify that the object exists.  Then check that it has
	 no descendents.  */

      /* Create the sql for determining whether the object exists and
	 whether it has any children.  */
      GString *sql = g_string_new ("");

      char *escaped = sqlite3_mprintf ("%Q", uuid);
      g_string_append_printf (sql, "begin transaction;"
			      "select uuid from %s where uuid = %s;",
			      table, escaped);

      int i;
      for (i = 0; child_tables && child_tables[i]; i ++)
	g_string_append_printf (sql, 
				"select uuid from %s where parent_uuid = %s;",
				child_tables[i], escaped);

      sqlite3_free (escaped);

      g_string_append_printf (sql, "end transaction;");

      /* If the object exists and has no children, exactly one row
	 will be returned.  If no rows are returned, then the object
	 does not exist.  If more than 1 row is returned, the object
	 has descendents.  */
      int count = 1;
      char *errmsg = NULL;
      int err = sqlite3_exec (db, sql->str,
			      abort_if_too_many_callback, &count, &errmsg);
      g_string_free (sql, TRUE);

      int ret = 0;
      if (err == SQLITE_ABORT)
	{
	  if (errmsg)
	    {
	      sqlite3_free (errmsg);
	      errmsg = NULL;
	    }

	  g_set_error (error, G_MURMELTIER_ERROR, 0,
		       "%s has descendents, not removing.", uuid);
	  ret = WOODCHUCK_ERROR_OBJECT_EXISTS;
	  sqlite3_exec (db, "rollback transaction;", NULL, NULL, &errmsg);
	  if (errmsg)
	    {
	      debug (0, "Aborting transaction: %s", errmsg);
	      sqlite3_free (errmsg);
	      errmsg = NULL;
	    }
	}
      else if (count == 1)
	ret = WOODCHUCK_ERROR_NO_SUCH_OBJECT;
      else if (errmsg)
	{
	  g_set_error (error, G_MURMELTIER_ERROR, 0,
		       "Internal error at %s:%d: %s",
		       __FILE__, __LINE__, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;

	  ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
	}

      if (ret)
	/* Something went wrong.  Abort.  */
	{
	  sqlite3_exec (db, "rollback transaction;",
			NULL, NULL, NULL);
	  return ret;
	}


      /* Do the deletion.  */
      sql = g_string_new ("");

      escaped = sqlite3_mprintf ("%Q", uuid);
      g_string_append_printf (sql, "begin transaction;"
			      "delete from %s where uuid = %s;",
			      table, escaped);

      for (i = 0; secondary_tables && secondary_tables[i]; i ++)
	g_string_append_printf (sql,
				"delete from %s where uuid = %s;",
				secondary_tables[i], escaped);

      for (i = 0; child_tables && child_tables[i]; i ++)
	g_string_append_printf (sql,
				"delete from %s where parent_uuid = %s;",
				child_tables[i], escaped);

      sqlite3_free (escaped);

      g_string_append_printf (sql, "end transaction;");


      int total_changes = sqlite3_total_changes (db);
      err = sqlite3_exec (db, sql->str, NULL, NULL, &errmsg);
      g_string_free (sql, TRUE);
      if (errmsg)
	{
	  g_set_error (error, G_MURMELTIER_ERROR, 0,
		       "Internal error at %s:%d: %s",
		       __FILE__, __LINE__, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	  return WOODCHUCK_ERROR_INTERNAL_ERROR;
	}

      int deleted_rows = sqlite3_total_changes (db) - total_changes;
      if (deleted_rows == 0)
	{
	  debug (0, "Expected to delete a row, but deleted %d. "
		 "DB inconsistent?",
		 deleted_rows);
	}

      return 0;
    }
  else
    {
      if (strcmp (table, "managers") == 0)
#warning Implement object_delete recursive for managers
	return WOODCHUCK_ERROR_NOT_IMPLEMENTED;

      /* Create the sql.  */
      GString *sql = g_string_new ("begin transaction;");

      char *escaped = sqlite3_mprintf ("%Q", uuid);

      g_string_append_printf (sql, 
			      "delete from %s where uuid = %s;",
			      table, escaped);

      int i;
      for (i = 0; secondary_tables && secondary_tables[i]; i ++)
	g_string_append_printf (sql, 
				"delete from %s where uuid = %s;",
				secondary_tables[i], escaped);
      for (i = 0; child_tables && child_tables[i]; i ++)
	g_string_append_printf (sql, 
				"delete from %s where parent_uuid = %s;",
				child_tables[i], escaped);

      sqlite3_free (escaped);

      g_string_append_printf (sql, "end transaction;");

      int total_changes = sqlite3_total_changes (db);
      char *errmsg = NULL;
      sqlite3_exec_printf (db, sql->str, NULL, NULL, &errmsg);
      g_string_free (sql, TRUE);

      if (errmsg)
	{
	  g_set_error (error, G_MURMELTIER_ERROR, 0,
		       "Internal error at %s:%d: %s",
		       __FILE__, __LINE__, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	  return WOODCHUCK_ERROR_INTERNAL_ERROR;
	}

      assert (total_changes != -1);

      int deleted_rows = sqlite3_total_changes (db) - total_changes;
      debug (0, "Removing %s removed %d objects.",
	     uuid, deleted_rows);
      if (deleted_rows == 0)
	{
	  g_set_error (error, G_MURMELTIER_ERROR, 0,
		       "Object '%s' does not exist", uuid);
	  return WOODCHUCK_ERROR_GENERIC;
	}

      return 0;
    }
}

enum woodchuck_error
woodchuck_manager_register (GHashTable *properties,
			    gboolean only_if_cookie_unique,
			    char **uuid, GError **error)
{
  return woodchuck_manager_manager_register
    (NULL, properties, only_if_cookie_unique, uuid, error);
}


enum woodchuck_error
woodchuck_list_managers (gboolean recursive,
			 GPtrArray **managers, GError **error)
{
  return woodchuck_manager_list_managers (NULL, recursive, managers, error);
}

enum woodchuck_error
woodchuck_lookup_manager_by_cookie (const char *cookie, gboolean recursive,
				    GPtrArray **managers, GError **error)
{
  return lookup_by ("managers", "Cookie", cookie, NULL, recursive,
		    "uuid, HumanReadableName, parent_uuid",
		    managers, error);
}

enum woodchuck_error
woodchuck_transfer_desirability
  (uint32_t request_type,
   struct woodchuck_transfer_desirability_version *versions, int version_count,
   uint32_t *desirability, uint32_t *version, GError **error)
{
#warning Implement woodchuck_transfer_desirability
  return WOODCHUCK_ERROR_NOT_IMPLEMENTED;
}

enum woodchuck_error
woodchuck_manager_manager_register (const char *manager, GHashTable *properties,
				    gboolean only_if_cookie_unique,
				    char **uuid, GError **error)
{
  const char *required_properties[] = { "HumanReadableName", NULL };
  return object_register (manager, "managers", "managers", properties,
			  manager_properties, required_properties,
			  only_if_cookie_unique, uuid, error);
}

enum woodchuck_error
woodchuck_manager_unregister (const char *manager, bool only_if_no_descendents,
			      GError **error)
{
  const char *child_tables[] = { "managers", "streams", "stream_updates",
				 NULL };
  return object_unregister (manager, "managers", NULL, child_tables,
			    only_if_no_descendents, error);
}

enum woodchuck_error
woodchuck_manager_list_managers
 (const char *manager, gboolean recursive, GPtrArray **managers, GError **error)
{
  *managers = g_ptr_array_new ();

  debug (0, "manager: %s, recursive: %d", manager, recursive);

  char *errmsg = NULL;
  int err;
  if (recursive && ! manager)
    /* List everything.  */
    err = sqlite3_exec
      (db,
       "select uuid, Cookie, HumanReadableName, parent_uuid from managers;",
       list_callback, *managers, &errmsg);
  else if (recursive)
    /* List only those that are descended from MANAGER.  */
    {
      int i = 0;
      for (;;)
	{
	  err = sqlite3_exec_printf
	    (db,
	     "select uuid, Cookie, HumanReadableName, parent_uuid"
	     " from managers where parent_uuid = %Q;",
	     list_callback, *managers, &errmsg, manager);
	  if (errmsg)
	    break;

	  debug (0, "Processed %d of %d managers", i, (*managers)->len);

	  if (i >= (*managers)->len)
	    break;
	  GPtrArray *strct = g_ptr_array_index ((*managers), i);
	  debug (0, "%d fields", strct->len);
	  manager = g_ptr_array_index (strct, 0);
	  i ++;
	}
    }
  else
    /* List only those that are an immediate descendent of MANAGER.  */
    err = sqlite3_exec_printf
      (db,
       "select uuid, Cookie, HumanReadableName, parent_uuid"
       " from managers where parent_uuid = %Q;",
       list_callback, *managers, &errmsg,
       manager ?: "");

  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      return WOODCHUCK_ERROR_INTERNAL_ERROR;
    }

  return 0;
}

enum woodchuck_error
woodchuck_manager_lookup_manager_by_cookie
  (const char *manager, const char *cookie, gboolean recursive,
   GPtrArray **managers, GError **error)
{
  return lookup_by ("managers", "Cookie", cookie, manager, recursive,
		    "uuid, HumanReadableName, parent_uuid",
		    managers, error);
}

enum woodchuck_error
woodchuck_manager_stream_register (const char *manager, GHashTable *properties,
				   gboolean only_if_cookie_unique,
				   char **uuid, GError **error)
{
  const char *required_properties[] = { "HumanReadableName", NULL };
  return object_register (manager, "managers", "streams", properties,
			  stream_properties, required_properties,
			  only_if_cookie_unique, uuid, error);
}

enum woodchuck_error
woodchuck_manager_list_streams
  (const char *manager, GPtrArray **list, GError **error)
{
  *list = g_ptr_array_new ();

  char *errmsg = NULL;
  int err;
  err = sqlite3_exec_printf
    (db,
     "select uuid, Cookie, HumanReadablename from streams"
     " where parent_uuid=%Q;",
     list_callback, *list, &errmsg, manager);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      return WOODCHUCK_ERROR_INTERNAL_ERROR;
    }

  return 0;
}

enum woodchuck_error
woodchuck_manager_lookup_stream_by_cookie
  (const char *manager, const char *cookie, GPtrArray **list, GError **error)
{
  return lookup_by ("streams", "Cookie", cookie, manager, FALSE,
		    "uuid, HumanReadableName",
		    list, error);
}


enum woodchuck_error
woodchuck_manager_feedback_subscribe
  (const char *sender, const char *manager, bool descendents_too, char **handle,
   GError **error)
{
  if (descendents_too)
#warning Support notifications for nested managers.
    return WOODCHUCK_ERROR_NOT_IMPLEMENTED;

  int sender_len = strlen (sender);
  int manager_len = strlen (manager);
  struct subscription *s = g_malloc (sizeof (*s) + sender_len + 1
				     + manager_len + 1
				     + sender_len + 1 + 16 + 1);

  s->proxy = dbus_g_proxy_new_for_name
    (mt->session_bus, sender, "/org/woodchuck", "org.woodchuck.upcall");

  s->descendents_too = descendents_too;

  char *p = s->data;
  s->dbus_name = p;
  p = mempcpy (p, sender, sender_len + 1);
  s->manager = p;
  p = mempcpy (p, manager, manager_len + 1);

  static uint64_t c;
  s->handle = p;
  snprintf (p, sender_len + 1 + 16, "%s.%"PRIx64, sender, c ++);
  /* Ensure that it is NUL terminated.  */
  p[sender_len + 1 + 16] = 0;


  /* Add the subscription to the various hashes.  */
  g_hash_table_insert (mt->handle_to_subscription_hash, s->handle, s);


  GSList *l = g_hash_table_lookup (mt->manager_to_subscription_list_hash,
				   manager);
  l = g_slist_prepend (l, s);
  g_hash_table_replace (mt->manager_to_subscription_list_hash, s->manager, l);


  l = g_hash_table_lookup (mt->bus_name_to_subscription_list_hash,
			   s->dbus_name);
  l = g_slist_prepend (l, s);
  g_hash_table_replace (mt->bus_name_to_subscription_list_hash,
			s->dbus_name, l);

  *handle = g_strdup (s->handle);

  schedule ();

  return 0;
}

enum woodchuck_error
woodchuck_manager_feedback_unsubscribe
  (const char *sender, const char *manager, const char *handle, GError **error)
{
  struct subscription *s
    = g_hash_table_lookup (mt->handle_to_subscription_hash, handle);
  if (! s)
    return WOODCHUCK_ERROR_NO_SUCH_OBJECT;

  /* Remove it from the handle to subscription hash.  */
  if (! g_hash_table_remove (mt->handle_to_subscription_hash, handle))
    {
      debug (0, "Failed to remove %s from handle_to_subscription_hash,"
	     "but just looked it up!",
	     handle);
      assert (0 == 1);
    }


  /* Remove it from the manager to subscription list hash.  */
  GSList *l = g_hash_table_lookup (mt->manager_to_subscription_list_hash,
				   manager);
  l = g_slist_remove (l, s);
  if (l)
    g_hash_table_replace (mt->manager_to_subscription_list_hash,
			  ((struct subscription *) l->data)->manager, l);
  else
    {
      if (! g_hash_table_remove (mt->manager_to_subscription_list_hash,
				 manager))
	{
	  debug (0, "Failed to remove %s from "
		 "manager_to_subscription_list_hash, but just looked it up!",
		 manager);
	  assert (0 == 1);
	}
    }

  /* Remove it from the bus name to subscription list hash.  */
  l = g_hash_table_lookup (mt->bus_name_to_subscription_list_hash,
			   s->dbus_name);
  l = g_slist_remove (l, s);
  if (l)
    g_hash_table_replace (mt->bus_name_to_subscription_list_hash,
			  ((struct subscription *) l->data)->dbus_name, l);
  else
    {
      if (! g_hash_table_remove (mt->bus_name_to_subscription_list_hash,
				 s->dbus_name))
	{
	  debug (0, "Failed to remove %s from "
		 "bus_name_to_subscription_list_hash, but just looked it up!",
		 s->dbus_name);
	  assert (0 == 1);
	}
    }


  g_free (s);

  return 0;
}

enum woodchuck_error
woodchuck_manager_feedback_ack
  (const char *sender, const char *manager,
   const char *object_uuid, uint32_t instance, GError **error)
{
#warning Implement woodchuck_manager_feedback_ack
  return WOODCHUCK_ERROR_NOT_IMPLEMENTED;
}

enum woodchuck_error
woodchuck_stream_unregister (const char *stream, bool only_if_empty,
			     GError **error)
{
  const char *child_tables[] = { "objects", "object_versions",
				 "object_instance_status",
				 "object_instance_files",
				 "object_use",
				 NULL };
  const char *secondary_tables[] = { "stream_updates", NULL };
  return object_unregister (stream, "streams", secondary_tables, child_tables,
			    only_if_empty, error);
}

enum woodchuck_error
woodchuck_stream_object_register (const char *stream, GHashTable *properties,
				  gboolean only_if_cookie_unique,
				  char **uuid, GError **error)
{
  const char *required_properties[] = { "HumanReadableName", NULL };
  return object_register (stream, "streams", "objects", properties,
			  object_properties, required_properties,
			  only_if_cookie_unique, uuid, error);
}

enum woodchuck_error
woodchuck_stream_list_objects (const char *stream,
			       GPtrArray **list, GError **error)
{
  *list = g_ptr_array_new ();

  char *errmsg = NULL;
  int err;
  err = sqlite3_exec_printf
    (db,
     "select uuid, Cookie, HumanReadableName from objects"
     " where parent_uuid=%Q;",
     list_callback, *list, &errmsg, stream);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      return WOODCHUCK_ERROR_INTERNAL_ERROR;
    }

  return 0;
}

enum woodchuck_error
woodchuck_stream_update_status
  (const char *stream_raw, uint32_t status, uint32_t indicator,
   uint64_t transferred_up, uint64_t transferred_down,
   uint64_t transfer_time, uint32_t transfer_duration, 
   uint32_t new_objects, uint32_t updated_objects,
   uint32_t objects_inline, GError **error)
{
  char *stream = sqlite3_mprintf ("%Q", stream_raw);
  char *manager = NULL;

  uint64_t n = now ();

  if (transfer_time == 0 || transfer_time > n / 1000)
    transfer_time = n / 1000;

  debug (4, DEBUG_BOLD ("stream: %s; status: %"PRIx32"; indicator: %"PRIx32"; "
			"transferred: "BYTES_FMT"/"BYTES_FMT"; "
			"transfer: "TIME_FMT"/"TIME_FMT"; "
			"objects: %"PRId32";%"PRId32";%"PRId32),
	 stream_raw, status, indicator,
	 BYTES_PRINTF (transferred_up), BYTES_PRINTF (transferred_down),
	 TIME_PRINTF (n - 1000 * transfer_time),
	 TIME_PRINTF (1000 * (uint64_t) transfer_duration),
	 new_objects, updated_objects, objects_inline);

  int instance = -1;
  int callback (void *cookie, int argc, char **argv, char **names)
  {
    assert (instance == -1);
    assert (manager == NULL);
    instance = argv[0] ? atoi (argv[0]) : 0;
    manager = g_strdup (argv[1]);
    return 0;
  }

  enum woodchuck_error ret = 0;

  char *errmsg = NULL;
  int err = sqlite3_exec_printf
    (db, "select instance, parent_uuid from streams where uuid = %s;",
     callback, NULL, &errmsg, stream);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);

      sqlite3_free (errmsg);
      errmsg = NULL;

      ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
      goto out;
    }

  if (instance == -1)
    {
      ret = WOODCHUCK_ERROR_NO_SUCH_OBJECT;
      goto out;
    }

  err = sqlite3_exec_printf
    (db,
     "begin transaction;\n"
     "insert into stream_updates"
     " (uuid, instance, parent_uuid,"
     "  status, indicator, transferred_up, transferred_down,"
     "  transfer_time, transfer_duration,"
     "  new_objects, updated_objects, objects_inline)"
     " values"
     " (%s, %d, '%s',"
     "  %"PRId32", %"PRId32", %"PRId64", %"PRId64", %"PRId64", %"PRId32","
     "  %"PRId32", %"PRId32", %"PRId32");\n"
     "update streams set instance = %d where uuid = %s;\n"
     "end transaction;",
     NULL, NULL, &errmsg,
     stream, instance, manager, status, indicator,
     transferred_up, transferred_down, transfer_time, transfer_duration,
     new_objects, updated_objects, objects_inline,
     instance + 1, stream);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      sqlite3_exec (db, "rollback transaction;\n", NULL, NULL, NULL);

      ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
      goto out;
    }

 out:
  sqlite3_free (stream);
  g_free (manager);

  return ret;
}

enum woodchuck_error
woodchuck_object_unregister (const char *object, GError **error)
{
  const char *secondary_tables[] = { "object_versions",
				     "object_instance_status",
				     "object_instance_files",
				     "object_use",
				     NULL };
  return object_unregister (object, "objects", secondary_tables, NULL,
			    TRUE, error);
}

enum woodchuck_error
woodchuck_object_transfer (const char *object, uint32_t request_type,
			   GError **error)
{
#warning Implement woodchuck_object_transfer
  return WOODCHUCK_ERROR_NOT_IMPLEMENTED;
}

enum woodchuck_error
woodchuck_stream_lookup_object_by_cookie
  (const char *stream, const char *cookie, GPtrArray **list, GError **error)
{
  return lookup_by ("objects", "Cookie", cookie, stream, FALSE,
		    "uuid, HumanReadableName",
		    list, error);
}

enum woodchuck_error
woodchuck_object_transfer_status
  (const char *object_raw, uint32_t status, uint32_t indicator,
   uint64_t transferred_up, uint64_t transferred_down,
   uint64_t transfer_time, uint32_t transfer_duration, uint64_t object_size,
   struct woodchuck_object_transfer_status_files *files, int files_count,
   GError **error)
{
  char *object = sqlite3_mprintf ("%Q", object_raw);
  char *stream = NULL;

  uint64_t n = now ();
  if (transfer_time == 0 || transfer_time > n / 1000)
    transfer_time = n / 1000;

  int instance = -1;
  int callback (void *cookie, int argc, char **argv, char **names)
  {
    assert (instance == -1);
    assert (stream == NULL);
    instance = argv[0] ? atoi (argv[0]) : 0;
    stream = g_strdup (argv[1]);
    return 0;
  }

  enum woodchuck_error ret = 0;

  char *errmsg = NULL;
  int err = sqlite3_exec_printf
    (db, "select instance, parent_uuid from objects where uuid = %s;",
     callback, NULL, &errmsg, object);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
      goto out;
    }

  if (instance == -1)
    {
      ret = WOODCHUCK_ERROR_NO_SUCH_OBJECT;
      goto out;
    }

  GString *sql = NULL;

  if (files_count > 0)
    {
      sql = g_string_new ("");
      int i;
      for (i = 0; i < files_count; i ++)
	{
	  char *filename_escaped = sqlite3_mprintf ("%Q", files[i].filename);
	  g_string_append_printf
	    (sql,
	     "insert into object_instance_files"
	     " (uuid, instance, parent_uuid,"
	     "  filename, dedicated, deletion_policy)"
	     " values (%s, %d, '%s', %s, %d, %d);\n",
	     object, instance, stream,
	     filename_escaped, files[i].dedicated, files[i].deletion_policy);
	  sqlite3_free (filename_escaped);
	}
    }

  err = sqlite3_exec_printf
    (db,
     "begin transaction;\n"
     "insert into object_instance_status"
     " (uuid, instance, parent_uuid,"
     "  status, transferred_up, transferred_down,"
     "  transfer_time, transfer_duration, object_size, indicator)"
     " values"
     "  (%s, %d, '%s',"
     "   %"PRId32", %"PRId64", %"PRId64", %"PRId64", %"PRId32","
     "   %"PRId64", %"PRId32");\n"
     "%s"
     "update objects set instance = %d, NeedUpdate = 0 where uuid = %s;"
     "end transaction;",
     NULL, NULL, &errmsg,
     object, instance, stream, status, transferred_up, transferred_down,
     transfer_time, transfer_duration, object_size, indicator,
     sql ? sql->str : "", instance + 1, object);
  if (sql)
    g_string_free (sql, TRUE);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      sqlite3_exec (db, "rollback transaction;\n", NULL, NULL, NULL);

      ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
      goto out;
    }

 out:
  sqlite3_free (object);
  g_free (stream);

  return ret;
}

enum woodchuck_error
woodchuck_object_use (const char *object_raw, uint64_t start, uint64_t duration,
		      uint64_t use_mask, GError **error)
{
  char *object = sqlite3_mprintf ("%Q", object_raw);
  char *stream = NULL;

  int instance = -1;
  int callback (void *cookie, int argc, char **argv, char **names)
  {
    assert (instance == -1);
    assert (stream == NULL);
    instance = argv[0] ? atoi (argv[0]) : 0;
    stream = g_strdup (argv[1]);
    return 0;
  }

  enum woodchuck_error ret = 0;

  char *errmsg = NULL;
  int err = sqlite3_exec_printf
    (db, "select instance, parent_uuid from objects where uuid = %s;",
     callback, NULL, &errmsg, object);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
      goto out;
    }

  if (instance == -1)
    {
      ret = WOODCHUCK_ERROR_NO_SUCH_OBJECT;
      goto out;
    }

  err = sqlite3_exec_printf
    (db,
     "insert into object_use"
     " (uuid, instance, parent_uuid, reported, start, duration, use_mask)"
     " values"
     " (%s, %d, '%s', 1, %"PRId64", %"PRId64", %"PRId64");",
     NULL, NULL, &errmsg,
     object, instance, stream, start, duration, use_mask);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
      goto out;
    }

 out:
  sqlite3_free (object);
  g_free (stream);

  return ret;
}

enum woodchuck_error
woodchuck_object_files_deleted (const char *object_raw,
				uint32_t update, uint64_t arg,
				GError **error)
{
  char *object = sqlite3_mprintf ("%Q", object_raw);
  char *stream = NULL;

  int instance = -1;
  int callback (void *cookie, int argc, char **argv, char **names)
  {
    assert (instance == -1);
    assert (stream == NULL);
    instance = argv[0] ? atoi (argv[0]) : 0;
    stream = g_strdup (argv[1]);
    return 0;
  }

  enum woodchuck_error ret = 0;

  char *errmsg = NULL;
  int err = sqlite3_exec_printf
    (db, "select instance, parent_uuid from objects where uuid = %s;",
     callback, NULL, &errmsg, object);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
      goto out;
    }

  if (instance == -1)
    {
      ret = WOODCHUCK_ERROR_NO_SUCH_OBJECT;
      goto out;
    }

  char *sql = NULL;
  switch (update)
    {
    case WOODCHUCK_DELETE_DELETED:
      sql = sqlite3_mprintf ("deleted = 1");
      break;
      
    case WOODCHUCK_DELETE_COMPRESSED:
      sql = sqlite3_mprintf ("compressed_size = %"PRId64, arg);
      break;

    case WOODCHUCK_DELETE_REFUSED:
      sql = sqlite3_mprintf ("preserve_until = %"PRId64, time (NULL) + arg);
      break;

    default:
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Bad value for Update argument: %d", update);
      ret = WOODCHUCK_ERROR_INVALID_ARGS;
      goto out;
    }

  err = sqlite3_exec_printf
    (db,
     "update object_instance_status set %s"
     " where uuid = %s"
     " and instance"
     "  = (select max (instance) from object_instance_status where uuid = %s);",
     NULL, NULL, &errmsg, sql, object, object);
  sqlite3_free (sql);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      ret = WOODCHUCK_ERROR_INTERNAL_ERROR;
      goto out;
    }

 out:
  sqlite3_free (object);
  g_free (stream);

  return ret;
}

static enum woodchuck_error
property_get (const char *object,
	      const char *table, struct property *properties,
	      const char *expected_interface_name,
	      const char *interface_name, const char *property_name,
	      GValue *value, GError **error)
{
  int i;
  for (i = 0; properties && properties[i].name; i ++)
    if (strcmp (properties[i].name, property_name) == 0)
      break;

  if (! properties || ! properties[i].name
      || ! (*interface_name == '\0'
	    || strcmp (interface_name, expected_interface_name) == 0))
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "No such property: %s%s%s",
		   interface_name, *interface_name ? "." : "", property_name);
      return DBUS_GERROR_INVALID_ARGS;
    }

  int callback (void *cookie, int argc, char **argv, char **names)
  {
    debug (4, "Properties.Get ('%s', '%s') -> %s",
	   interface_name, property_name, argv[0]);

    char *tailptr = NULL;
    switch (properties[i].type)
      {
      default:
	debug (0, "Property %s has unhandled type (%d)!",
	       property_name, (int) properties[i].type);
      case G_TYPE_STRING:
	g_value_init (value, G_TYPE_STRING);
	if (! argv[0])
	  g_value_set_static_string (value, "");
	else
	  g_value_set_string (value, argv[0]);
	break;
      case G_TYPE_BOOLEAN:
	g_value_init (value, G_TYPE_BOOLEAN);
	g_value_set_boolean (value,
			     argv[0] ? strtol (argv[0], &tailptr, 10) : 0);
	break;
      case G_TYPE_INT:
	g_value_init (value, G_TYPE_INT);
	g_value_set_int (value, argv[0] ? strtol (argv[0], &tailptr, 10) : 0);
	break;
      case G_TYPE_UINT:
	g_value_init (value, G_TYPE_UINT);
	g_value_set_uint (value,
			  argv[0] ? strtoul (argv[0], &tailptr, 10) : 0);
	break;
      case G_TYPE_INT64:
	g_value_init (value, G_TYPE_INT64);
	g_value_set_int64 (value,
			   argv[0] ? strtoll (argv[0], &tailptr, 10) : 0);
	break;
      case G_TYPE_UINT64:
	g_value_init (value, G_TYPE_UINT64);
	g_value_set_uint64 (value,
			    argv[0] ? strtoull (argv[0], &tailptr, 10) : 0);
	break;
      }
    return 0;
  }

  char *errmsg = NULL;
  sqlite3_exec_printf
    (db, "select %s from %s where uuid = '%s';",
     callback, NULL, &errmsg, property_name, table, object);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      return WOODCHUCK_ERROR_INTERNAL_ERROR;
    }

  return 0;
}

static enum woodchuck_error
property_set (const char *object,
	      const char *table, struct property *properties,
	      const char *expected_interface_name,
	      const char *interface_name, const char *property_name,
	      GValue *value, GError **error)
{
  int i;
  for (i = 0; properties && properties[i].name; i ++)
    if (strcmp (properties[i].name, property_name) == 0)
      break;

  if (! properties || ! properties[i].name
      || ! (*interface_name == '\0'
	    || strcmp (interface_name, expected_interface_name) == 0))
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "No such property: %s%s%s",
		   interface_name, *interface_name ? "." : "", property_name);
      return DBUS_GERROR_INVALID_ARGS;
    }

  if (! properties[i].readwrite)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Can't set readonly property: %s%s%s",
		   interface_name, *interface_name ? "." : "", property_name);
      return DBUS_GERROR_INVALID_ARGS;
    }

  if (properties[i].type != G_VALUE_TYPE (value))
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Type mismatch setting %s%s%s",
		   interface_name, *interface_name ? "." : "", property_name);
      return DBUS_GERROR_INVALID_ARGS;
    }

  char *escaped_value = NULL;
  switch (G_VALUE_TYPE (value))
    {
    case G_TYPE_STRING:
      escaped_value = sqlite3_mprintf ("%Q", g_value_get_string (value));
      break;
    case G_TYPE_INT:
      escaped_value = sqlite3_mprintf ("%d", g_value_get_int (value));
      break;
    case G_TYPE_UINT:
      escaped_value = sqlite3_mprintf ("%u", g_value_get_uint (value));
      break;
    case G_TYPE_INT64:
      escaped_value = sqlite3_mprintf ("%"PRId64, g_value_get_int64 (value));
      break;
    case G_TYPE_UINT64:
      escaped_value = sqlite3_mprintf ("%"PRIu64, g_value_get_uint64 (value));
      break;
    case G_TYPE_BOOLEAN:
      escaped_value = sqlite3_mprintf ("%d", g_value_get_boolean (value));
      break;
    default:
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "%s:%d: Property %s has unhandled type (%d)!",
		   __FILE__, __LINE__,
		   property_name, (int) G_VALUE_TYPE (value));
      return WOODCHUCK_ERROR_INTERNAL_ERROR;
    }

  char *errmsg = NULL;
  int err = sqlite3_exec_printf
    (db, "update %s set %s = %s where uuid = '%s'",
     NULL, NULL, &errmsg,
     table, property_name, escaped_value, object);
  sqlite3_free (escaped_value);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      return WOODCHUCK_ERROR_INTERNAL_ERROR;
    }

  return 0;
}

enum woodchuck_error
woodchuck_property_get (const char *object,
			const char *interface_name, const char *property_name,
			GValue *value, GError **error)
{
  return property_get (NULL, NULL, NULL,
		       "org.woodchuck", interface_name, property_name,
		       value, error);
}

enum woodchuck_error
woodchuck_property_set (const char *object,
			const char *interface_name, const char *property_name,
			GValue *value, GError **error)
{
  return property_set (NULL, NULL, NULL,
		       "org.woodchuck", interface_name, property_name,
		       value, error);
}

enum woodchuck_error
woodchuck_manager_property_get (const char *object, const char *interface_name,
				const char *property_name,
				GValue *value, GError **error)
{
  return property_get (object, "managers", manager_properties,
		       "org.woodchuck.manager", interface_name, property_name,
		       value, error);
}

enum woodchuck_error
woodchuck_manager_property_set (const char *object, const char *interface_name,
				const char *property_name,
				GValue *value, GError **error)
{
  return property_set (object, "managers", manager_properties,
		       "org.woodchuck.manager", interface_name, property_name,
		       value, error);
}

enum woodchuck_error
woodchuck_stream_property_get (const char *object, const char *interface_name,
			       const char *property_name,
			       GValue *value, GError **error)
{
  return property_get (object, "streams", stream_properties,
		       "org.woodchuck.stream", interface_name, property_name,
		       value, error);
}

enum woodchuck_error
woodchuck_stream_property_set (const char *object, const char *interface_name,
			       const char *property_name,
			       GValue *value, GError **error)
{
  return property_set (object, "streams", stream_properties,
		       "org.woodchuck.stream", interface_name, property_name,
		       value, error);
}

enum woodchuck_error
woodchuck_object_property_get (const char *object, const char *interface_name,
			       const char *property_name,
			       GValue *value, GError **error)
{
#warning Support getting and setting object.Versions
  return property_get (object, "objects", object_properties,
		       "org.woodchuck.object", interface_name, property_name,
		       value, error);
}

enum woodchuck_error
woodchuck_object_property_set (const char *object, const char *interface_name,
			       const char *property_name,
			       GValue *value, GError **error)
{
  return property_set (object, "objects", object_properties,
		       "org.woodchuck.object", interface_name, property_name,
		       value, error);
}

int
main (int argc, char *argv[])
{
  g_type_init();

  int err = dotdir_init ("murmeltier");
  if (err)
    {
      debug (0, "dotdir_init ('murmeltier'): %m");
      return 1;
    }

  /* Open the DB.  */
  char *filename = dotdir_filename (NULL, "config.db");
  err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  char *errmsg = NULL;
  err = sqlite3_exec
    (db,
     "create table if not exists managers"
     " (uuid PRIMARY KEY, parent_uuid NOT NULL, HumanReadableName,"
     "  DBusServiceName, DBusObject, Cookie, Priority,"
     "  RegistrationTime DEFAULT (strftime ('%s', 'now')));"
     "create index if not exists managers_cookie_index on managers (cookie);"
     "create index if not exists managers_parent_uuid_index on managers"
     " (parent_uuid);"

     "create table if not exists streams"
     " (uuid PRIMARY KEY, parent_uuid NOT NULL, instance,"
     "  HumanReadableName, Cookie, Priority, Freshness, ObjectsMostlyInline,"
     "  RegistrationTime DEFAULT (strftime ('%s', 'now')));"
     "create index if not exists streams_cookie_index on streams (cookie);"
     "create index if not exists streams_parent_uuid_index"
     " on streams (parent_uuid);"

     "create table if not exists stream_updates"
     " (uuid NOT NULL, instance, parent_uuid NOT NULL,"
     "  status, indicator, transferred_up, transferred_down,"
     "  transfer_time, transfer_duration,"
     "  new_objects, updated_objects, objects_inline,"
     "  UNIQUE (uuid, instance));"
     "create index if not exists stream_updates_parent_uuid_index"
     " on stream_updates (parent_uuid);"

     "create table if not exists objects"
     " (uuid PRIMARY KEY, parent_uuid NOT NULL,"
     "  Instance DEFAULT 0, HumanReadableName, Cookie, Filename, Wakeup,"
     "  TriggerTarget, TriggerEarliest, TriggerLatest,"
     "  TransferFrequency,"
     "  DontTransfer DEFAULT 0, NeedUpdate, Priority,"
     "  DiscoveryTime, PublicationTime,"
     "  RegistrationTime DEFAULT (strftime ('%s', 'now')));"
     "create index if not exists objects_cookie_index on objects (cookie);"
     "create index if not exists objects_parent_uuid_index"
     " on objects (parent_uuid);"

     /* The available versions of an object.  Columns are as per the
	org.woodchuck.Object.Versions property.  */
     "create table if not exists object_versions"
     " (uuid NOT NULL, version NOT NULL, parent_uuid NOT NULL,"
     "  url, expected_size, expected_transfer_up, expected_transfer_down,"
     "  utility, use_simple_transferer,"
     "  UNIQUE (uuid, version, url));"
     "create index if not exists object_versions_parent_uuid_index"
     " on object_versions (parent_uuid);"

     /* An object instance's status.  Columns are as per
	org.woodchuck.object.TransferStatus.  */
     "create table if not exists object_instance_status"
     " (uuid NOT NULL, instance NOT NULL, parent_uuid NOT NULL,"
     "  status, transferred_up, transferred_down,"
     "  transfer_time, transfer_duration, object_size, indicator,"
     "  deleted, preserve_until, compressed_size,"
     "  UNIQUE (uuid, instance));"
     "create index if not exists object_status_parent_uuid_index"
     " on object_instance_status (parent_uuid);"

     "create table if not exists object_instance_files"
     " (uuid NOT NULL, instance NOT NULL, parent_uuid NOT NULL,"
     "  filename, dedicated, deletion_policy,"
     "  UNIQUE (uuid, instance, filename));"
     "create index if not exists object_instance_files_parent_uuid_index"
     " on object_instance_files (parent_uuid);"

     "create table if not exists object_use"
     " (uuid NOT NULL, instance NOT NULL, parent_uuid NOT NULL,"
     "  reported, start, duration, use_mask);"
     "create index if not exists object_use_parent_uuid_index"
     " on object_use (parent_uuid);",
     NULL, NULL, &errmsg);
  if (errmsg)
    {
      if (err != SQLITE_ABORT)
	debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
      return 1;
    }

  properties_init ();
  murmeltier_dbus_server_init ();

  mt = MURMELTIER (g_object_new (MURMELTIER_TYPE, NULL));
  if (! mt)
    {
      debug (0, "Failed to allocate memory.");
      abort ();
    }

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  return 0;
}
