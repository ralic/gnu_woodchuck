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

#include <assert.h>
#include <stdio.h>
#include <error.h>
#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>
#include <sqlite3.h>

#include "murmeltier-dbus-server.h"

#include "debug.h"
#include "util.h"
#include "dotdir.h"

#define G_MURMELTIER_ERROR murmeltier_error_quark ()
static GQuark
murmeltier_error_quark (void)
{
  return g_quark_from_static_string ("murmeltier");
}

static sqlite3 *db = NULL;

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

  NCNetworkMonitor *nm;

  sqlite3 *connectivity_db;
};

extern GType murmeltier_get_type (void);

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
connection_dump (NCNetworkConnection *nc)
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
connections_dump (gpointer user_data)
{
  Murmeltier *mt = MURMELTIER (user_data);
  NCNetworkMonitor *m = mt->nm;

  GList *e = nc_network_monitor_connections (m);
  while (e)
    {
      NCNetworkConnection *c = e->data;
      GList *n = e->next;
      g_list_free_1 (e);
      e = n;

      connection_dump (c);
    }

  return true;
}

/* A new connection has been established.  */
static void
new_connection (NCNetworkMonitor *nm, NCNetworkConnection *nc,
		gpointer user_data)
{
  printf (DEBUG_BOLD ("New %sconnection!!!")"\n",
	  nc_network_connection_is_default (nc) ? "DEFAULT " : "");

  connection_dump (nc);
}

/* An existing connection has been brought down.  */
static void
disconnected (NCNetworkMonitor *nm, NCNetworkConnection *nc,
	      gpointer user_data)
{
  printf ("\nDisconnected!!!\n\n");
}

/* There is a new default connection.  */
static void
default_connection_changed (NCNetworkMonitor *nm,
			    NCNetworkConnection *old_default,
			    NCNetworkConnection *new_default,
			    gpointer user_data)
{
  printf (DEBUG_BOLD ("Default connection changed!!!")"\n");
}

static void
murmeltier_init (Murmeltier *mt)
{
  // MurmeltierClass *klass = MURMELTIER_GET_CLASS (mt);

  /* Initialize the network monitor.  */
  mt->nm = nc_network_monitor_new ();

  g_signal_connect (G_OBJECT (mt->nm), "new-connection",
		    G_CALLBACK (new_connection), mt);
  g_signal_connect (G_OBJECT (mt->nm), "disconnected",
		    G_CALLBACK (disconnected), mt);
  g_signal_connect (G_OBJECT (mt->nm), "default-connection-changed",
		    G_CALLBACK (default_connection_changed), mt);

  g_timeout_add_seconds (5 * 60, connections_dump, mt);
}

static enum woodchuck_error
object_register (const char *parent, const char *parent_table,
		 const char *object_table, GHashTable *properties,
		 char *acceptable_properties[], char *required_properties[],
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

  debug (0, "Parent: '%s' (%p)", parent, parent);
  debug (0, "Properties:");
  void iter (gpointer keyp, gpointer valuep, gpointer user_data)
  {
    char *key = keyp;
    GValue *value = valuep;

    int i;
    for (i = 0; acceptable_properties[i]; i ++)
      if (strcmp (key, acceptable_properties[i]) == 0)
	break;

    if (! acceptable_properties[i])
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
	    static GType astub;
	    if (! astub)
	      astub = dbus_g_type_get_collection
		("GPtrArray",
		 dbus_g_type_get_struct ("GValueArray",
					 G_TYPE_STRING, G_TYPE_UINT64,
					 G_TYPE_UINT, G_TYPE_BOOLEAN,
					 G_TYPE_INVALID));

	    if (! G_VALUE_HOLDS (value, astub))
	      {
		debug (0, "Versions does not have type astub.");
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
	 "select uuid from %s where cookie = '%s';",
	 unique_callback, NULL, &errmsg, object_table, cookie);
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

	  uint64_t expected_size
	    = g_value_get_uint64 (g_value_array_get_nth (strct, 1));
	  uint32_t utility
	    = g_value_get_uint (g_value_array_get_nth (strct, 2));
	  gboolean use_simple_downloader
	    = g_value_get_boolean (g_value_array_get_nth (strct, 3));

	  g_string_append_printf
	    (sql,
	     "insert into object_versions"
	     " (uuid, version, parent_uuid,"
	     "  url, expected_size, utility, use_simple_downloader)"
	     " values"
	     " ('%s', %d, '%s', %s, %"PRId64", %d, %d);\n",
	     *uuid, i, parent, url_escaped, expected_size,
	     utility, use_simple_downloader);

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
 out:
  if (abort_transaction)
    {
      err = sqlite3_exec (db, "abort transaction", NULL, NULL, &errmsg);
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

  return 0;
}

static int
abort_if_too_many_callback (void *cookie, int argc, char **argv, char **names)
{
  int *count = cookie;
  if (count == 0)
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
	  ret = WOODCHUCK_ERROR_GENERIC;
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
	  sqlite3_exec (db, "abort transaction;",
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
woodchuck_download_desirability_version
  (uint32_t request_type,
   struct woodchuck_download_desirability_version *versions, int version_count,
   uint32_t *desirability, uint32_t *version, GError **error)
{
#warning Implement woodchuck_download_desirability_version
  return WOODCHUCK_ERROR_NOT_IMPLEMENTED;
}

enum woodchuck_error
woodchuck_manager_manager_register (const char *manager, GHashTable *properties,
				    gboolean only_if_cookie_unique,
				    char **uuid, GError **error)
{
  char *acceptable_properties[] = { "HumanReadableName", "DBusServiceName",
				    "DBusObject", "Cookie", "Priority",
				    NULL };
  char *required_properties[] = { "HumanReadableName", NULL };
  return object_register (manager, "managers", "managers", properties,
			  acceptable_properties, required_properties,
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
  char *acceptable_properties[]
    = { "HumanReadableName", "Cookie", "Priority", "Freshness",
	"ObjectsMostlyInline", NULL };
  char *required_properties[] = { "HumanReadableName", NULL };
  return object_register (manager, "managers", "streams", properties,
			  acceptable_properties, required_properties,
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
  (const char *manager, bool descendents_too, char **handle, GError **error)
{
#warning Implement woodchuck_manager_feedback_subscribe
  return WOODCHUCK_ERROR_NOT_IMPLEMENTED;
}

enum woodchuck_error
woodchuck_manager_feedback_unsubscribe
  (const char *manager, const char *handle, GError **error)
{
#warning Implement woodchuck_manager_feedback_unsubscribe
  return WOODCHUCK_ERROR_NOT_IMPLEMENTED;
}

enum woodchuck_error
woodchuck_manager_feedback_ack
  (const char *manager, const char *object_uuid, uint32_t instance,
   GError **error)
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
  char *acceptable_properties[]
    = { "HumanReadableName", "Cookie", "Versions", "Filename", "Wakeup",
	"TriggerTarget", "TriggerEarliest", "TriggerLatest",
	"DownloadFrequency", "Priority", NULL };
  char *required_properties[] = { "HumanReadableName", NULL };
  return object_register (stream, "streams", "objects", properties,
			  acceptable_properties, required_properties,
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
   uint64_t download_time, uint32_t download_duration, 
   uint32_t new_objects, uint32_t updated_objects,
   uint32_t objects_inline, GError **error)
{
  char *stream = sqlite3_mprintf ("%Q", stream_raw);
  char *manager = NULL;

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
     "  download_time, download_duration,"
     "  new_objects, updated_objects, objects_inline)"
     " values"
     " (%s, %d, '%s',"
     "  %"PRId32", %"PRId32", %"PRId64", %"PRId64", %"PRId64", %"PRId32","
     "  %"PRId32", %"PRId32", %"PRId32");\n"
     "update streams set instance = %d where uuid = %s;\n"
     "end transaction;",
     NULL, NULL, &errmsg,
     stream, instance, manager, status, indicator,
     transferred_up, transferred_down, download_time, download_duration,
     new_objects, updated_objects, objects_inline,
     instance + 1, stream);
  if (errmsg)
    {
      g_set_error (error, G_MURMELTIER_ERROR, 0,
		   "Internal error at %s:%d: %s",
		   __FILE__, __LINE__, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;

      sqlite3_exec (db, "abort transaction;\n", NULL, NULL, NULL);

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
woodchuck_object_download (const char *object, uint32_t request_type,
			   GError **error)
{
#warning Implement woodchuck_object_download
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
woodchuck_object_download_status
  (const char *object_raw, uint32_t status, uint32_t indicator,
   uint64_t transferred_up, uint64_t transferred_down,
   uint64_t download_time, uint32_t download_duration, uint64_t object_size,
   struct woodchuck_object_download_status_files *files, int files_count,
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
     "  download_time, download_duration, object_size, indicator)"
     " values"
     "  (%s, %d, '%s',"
     "   %"PRId32", %"PRId64", %"PRId64", %"PRId64", %"PRId32","
     "   %"PRId64", %"PRId32");\n"
     "%s"
     "update objects set instance = %d where uuid = %s;"
     "end transaction;",
     NULL, NULL, &errmsg,
     object, instance, stream, status, transferred_up, transferred_down,
     download_time, download_duration, object_size, indicator,
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

      sqlite3_exec (db, "abort transaction;\n", NULL, NULL, NULL);

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
     "  DBusServiceName, DBusObject, Cookie, Priority);"
     "create index if not exists managers_cookie_index on managers (cookie);"
     "create index if not exists managers_parent_uuid_index on managers"
     " (parent_uuid);"

     "create table if not exists streams"
     " (uuid PRIMARY KEY, parent_uuid NOT NULL, instance,"
     "  HumanReadableName, Cookie, Priority, Freshness, ObjectsMostlyInline);"
     "create index if not exists streams_cookie_index on streams (cookie);"
     "create index if not exists streams_parent_uuid_index"
     " on streams (parent_uuid);"

     "create table if not exists stream_updates"
     " (uuid NOT NULL, instance, parent_uuid NOT NULL,"
     "  status, indicator, transferred_up, transferred_down,"
     "  download_time, download_duration,"
     "  new_objects, updated_objects, objects_inline,"
     "  UNIQUE (uuid, instance));"
     "create index if not exists stream_updates_parent_uuid_index"
     " on stream_updates (parent_uuid);"

     "create table if not exists objects"
     " (uuid PRIMARY KEY, parent_uuid NOT NULL,"
     "  Instance DEFAULT 0, HumanReadableName, Cookie, Filename, Wakeup,"
     "  TriggerTarget, TriggerEarliest, TriggerLatest,"
     "  DownloadFrequency, Priority);"
     "create index if not exists objects_cookie_index on objects (cookie);"
     "create index if not exists objects_parent_uuid_index"
     " on objects (parent_uuid);"

     /* The available versions of an object.  Columns are as per the
	org.woodchuck.Object.Versions property.  */
     "create table if not exists object_versions"
     " (uuid NOT NULL, version NOT NULL, parent_uuid NOT NULL,"
     "  url, expected_size, utility, use_simple_downloader,"
     "  UNIQUE (uuid, version, url));"
     "create index if not exists object_versions_parent_uuid_index"
     " on object_versions (parent_uuid);"

     /* An object instance's status.  Columns are as per
	org.woodchuck.object.DownloadStatus.  */
     "create table if not exists object_instance_status"
     " (uuid NOT NULL, instance NOT NULL, parent_uuid NOT NULL,"
     "  status, transferred_up, transferred_down,"
     "  download_time, download_duration, object_size, indicator,"
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

  murmeltier_dbus_server_init ();

  Murmeltier *mt = MURMELTIER (g_object_new (MURMELTIER_TYPE, NULL));
  if (! mt)
    {
      debug (0, "Failed to allocate memory.");
      abort ();
    }

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  return 0;
}
