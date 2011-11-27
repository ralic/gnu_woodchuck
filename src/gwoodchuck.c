/* gwoodchuck.c - A woodchuck library that integrated with glib.
   Copyright (C) 2011 Neal H. Walfield <neal@walfield.org>

   Woodchuck is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   Woodchuck is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.  */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <error.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>

#include "woodchuck/gwoodchuck.h"

#include "org.woodchuck.h"
#include "org.woodchuck.manager.h"
#include "org.woodchuck.stream.h"
#include "org.woodchuck.object.h"

static gboolean org_woodchuck_upcall_object_transferred
  (GWoodchuck *wc, const char *manager_uuid, const char *manager_cookie,
   const char *stream_uuid, const char *stream_cookie,
   const char *object_uuid, const char *object_cookie,
   uint32_t status, uint32_t instance,
   GValueArray *version_strct, const char *filename, uint64_t object_size,
   uint64_t trigger_target, uint64_t trigger_fired,
   GError **error);

static gboolean org_woodchuck_upcall_stream_update (GWoodchuck *wc,
						    const char *manager_uuid,
						    const char *manager_cookie,
						    const char *stream_uuid,
						    const char *stream_cookie,
						    GError **error);

static gboolean org_woodchuck_upcall_object_transfer
  (GWoodchuck *wc, const char *manager_uuid, const char *manager_cookie,
   const char *stream_uuid, const char *stream_cookie,
   const char *object_uuid, const char *object_cookie,
   GValueArray *version_strct, const char *filename, uint32_t quality,
   GError **error);

static gboolean org_woodchuck_upcall_object_delete_files
  (GWoodchuck *wc, const char *manager_uuid, const char *manager_cookie,
   const char *stream_uuid, const char *stream_cookie,
   const char *object_uuid, const char *object_cookie,
   GPtrArray *filenames, GError **error);

#include "org.woodchuck.upcall.server-stubs.h"

#define G_WOODCHUCK_ERROR gwoodchuck_error_quark ()
static GQuark
gwoodchuck_error_quark (void)
{
  return g_quark_from_static_string ("gwoodchuck");
}

/* An object or a stream.  */
struct object
{
  char *human_readable_name;
  char *cookie;
  DBusGProxy *proxy;
  /* If a stream object, a hash from object identifiers to struct
     object *.  */
  GHashTable *hash;
  char data[];
};

struct _GWoodchuck
{
  GObject parent;

  /* Session bus.  */
  DBusGConnection *session_bus;

  /* woodchuck root proxy.  */
  DBusGProxy *woodchuck_proxy;
  /* manager proxy.  */
  DBusGProxy *manager_proxy;

  /* A hash from stream cookies to struct object *.  */
  GHashTable *stream_hash;

  struct gwoodchuck_vtable *vtable;
  gpointer user_data;
};

G_DEFINE_TYPE (GWoodchuck, gwoodchuck, G_TYPE_OBJECT);

static void
gwoodchuck_class_init (GWoodchuckClass *klass)
{
  gwoodchuck_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);

  dbus_g_object_type_install_info
    (GWOODCHUCK_TYPE, &dbus_glib_org_woodchuck_upcall_object_info);
}

static void
gwoodchuck_init (GWoodchuck *wc)
{
  GError *error = NULL;
  wc->session_bus = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
  if (error)
    {
      g_critical ("%s: Getting system bus: %s", __FUNCTION__, error->message);
      g_error_free (error);
      return;
    }

  wc->woodchuck_proxy = dbus_g_proxy_new_for_name
    (wc->session_bus,
     "org.woodchuck",
     "/org/woodchuck",
     "org.woodchuck");
}

GWoodchuck *
gwoodchuck_new (const char *human_readable_name,
		const char *dbus_service_name,
		struct gwoodchuck_vtable *vtable,
		gpointer user_data,
		GError **caller_error)
{
  GWoodchuck *wc = GWOODCHUCK (g_object_new (GWOODCHUCK_TYPE, NULL));
  char *uuid = NULL;

  wc->vtable = vtable;
  wc->user_data = user_data;

  GPtrArray *managers = NULL;
  GError *error = NULL;
  if (! org_woodchuck_lookup_manager_by_cookie (wc->woodchuck_proxy,
						dbus_service_name, FALSE,
						&managers, &error))
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      goto out;
    }

  if (managers->len > 1)
    /* There are multiple managers with the same cookie.  This is
       likely a programming bug.  Because it is not unambiguous which
       one is ours, there is little we can do.  To be on the safe
       side, we conservatively abort.  Recovering will require user
       intervention (remove a manager), but that is better than
       corruption...  */
    {
      GString *s = g_string_new ("");

      int i;
      for (i = 0; i < managers->len; i ++)
	{
	  GValueArray *strct = g_ptr_array_index (managers, i);

	  GValue *a_human_readable_name_value
	    = g_value_array_get_nth (strct, 1);
	  const char *a_human_readable_name
	    = g_value_get_string (a_human_readable_name_value);
	  g_string_append_printf (s, "%s'%s'",
				  i > 0 ? ", " : "",
				  a_human_readable_name);
	}
      g_set_error (caller_error,
		   G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_OBJECT_EXISTS,
		   "Multiple managers with dbus service name '%s' exist (%s).  "
		   "Aborting to avoid corruption.",
		   dbus_service_name, s->str);

      g_string_free (s, TRUE);

      goto out;
    }

  if (managers->len == 1)
    /* There is exactly one manager with the specified dbus service
       name.  We still check that the human readable name matches and
       if not conservatively abort.  */
    {
      GValueArray *strct = g_ptr_array_index (managers, 0);

      GValue *a_uuid_value = g_value_array_get_nth (strct, 0);
      const char *a_uuid = g_value_get_string (a_uuid_value);
      GValue *a_human_readable_name_value
	= g_value_array_get_nth (strct, 1);
      const char *a_human_readable_name
	= g_value_get_string (a_human_readable_name_value);

      if (strcmp (a_human_readable_name, human_readable_name) != 0)
	{
	  g_set_error (caller_error,
		       G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_OBJECT_EXISTS,
		       "A manager with dbus service name %s exist, "
		       "but different human readable name ('%s').  "
		       "Aborting to avoid corruption.",
		       dbus_service_name, a_human_readable_name);

	  goto out;
	}

      uuid = g_strdup (a_uuid);
    }

  if (! uuid)
    /* A manager with the supplied dbus service name and human
       readable name does not exist.  Create it.  */
    {
      assert (managers->len == 0);

      GHashTable *properties = g_hash_table_new (g_str_hash, g_str_equal);

      void adds (char *key, const char *value, GValue *gvalue)
      {
	memset (gvalue, 0, sizeof (*gvalue));
	g_value_init (gvalue, G_TYPE_STRING);
	g_value_set_static_string (gvalue, value);
	g_hash_table_insert (properties, key, gvalue);
      }

      GValue human_readable_name_value;
      adds ("HumanReadableName", human_readable_name,
	   &human_readable_name_value);
      GValue cookie_value;
      adds ("Cookie", dbus_service_name, &cookie_value);
      GValue dbus_service_name_value;
      adds ("DBusServiceName", dbus_service_name, &dbus_service_name_value);
      GValue dbus_object_value;
      adds ("DBusObject", "/org/woodchuck", &dbus_object_value);

      GError *error = NULL;
      gboolean ret = org_woodchuck_manager_register (wc->woodchuck_proxy,
						     properties, TRUE,
						     &uuid, &error);
      g_hash_table_unref (properties);

      if (! ret)
	{
	  g_prefix_error (&error, "%s: ", __FUNCTION__);
	  g_critical ("%s", error->message);
	  g_propagate_error (caller_error, error);
	  goto out;
	}
    }

  assert (uuid);

  /* We either found the manager or registered it.  */

  char *path = NULL;
  asprintf (&path, "/org/woodchuck/manager/%s", uuid);

  wc->manager_proxy = dbus_g_proxy_new_from_proxy (wc->woodchuck_proxy,
						   "org.woodchuck.manager",
						   path);

  free (path);

  wc->stream_hash = g_hash_table_new (g_str_hash, g_str_equal);


  /* Prepare to listen for feedback.  */

  dbus_g_connection_register_g_object (wc->session_bus,
				       "/org/woodchuck",
				       G_OBJECT (wc));

 out:
  g_free (uuid);

  if (managers)
    {
      int i;
      for (i = 0; i < managers->len; i ++)
	g_value_array_free (g_ptr_array_index (managers, i));
      g_ptr_array_free (managers, TRUE);
    }

  if (error)
    {
      g_object_unref (wc);
      wc = NULL;
    }

  return wc;
}

/* Register a stream in the local lookup table.  */
static struct object *
stream_register_local (DBusGProxy *proxy, GHashTable *hash, const char *uuid,
		       const char *cookie, const char *human_readable_name)
{
  char *path = NULL;
  asprintf (&path, "/org/woodchuck/stream/%s", uuid);

  int human_readable_name_len = strlen (human_readable_name);
  int cookie_len = strlen (cookie);
  struct object *stream = g_malloc0 (sizeof (*stream)
				     + human_readable_name_len + 1
				     + cookie_len + 1);

  char *p = stream->data;
  stream->human_readable_name = p;
  p = mempcpy (p, human_readable_name, human_readable_name_len + 1);
  stream->cookie = p;
  p = mempcpy (p, cookie, cookie_len + 1);

  stream->proxy = dbus_g_proxy_new_from_proxy (proxy,
					       "org.woodchuck.stream",
					       path);
  free (path);

  g_hash_table_insert (hash, stream->cookie, stream);

  stream->hash = g_hash_table_new (g_str_hash, g_str_equal);

  return stream;
}

/* Register an object in the local lookup table.  */
static struct object *
object_register_local (DBusGProxy *proxy, GHashTable *hash, const char *uuid,
		       const char *cookie, const char *human_readable_name)
{
  char *path = NULL;
  asprintf (&path, "/org/woodchuck/object/%s", uuid);

  int human_readable_name_len = strlen (human_readable_name);
  int cookie_len = strlen (cookie);
  struct object *object = g_malloc0 (sizeof (*object)
				     + human_readable_name_len + 1
				     + cookie_len + 1);

  char *p = object->data;
  object->human_readable_name = p;
  p = mempcpy (p, human_readable_name, human_readable_name_len + 1);
  object->cookie = p;
  p = mempcpy (p, cookie, cookie_len + 1);

  object->proxy = dbus_g_proxy_new_from_proxy (proxy,
					       "org.woodchuck.object",
					       path);
  free (path);

  g_hash_table_insert (hash, object->cookie, object);

  return object;
}

/* Return the UUID of the object with the cookie COOKIE, or NULL if it
   does not exist.  If an error occurs, *CALLER_ERROR will be set
   (i.e., you probably don't want to pass NULL).  */
static struct object *
ll_object_lookup (DBusGProxy *proxy, GHashTable *hash,
		  const char *cookie,
		  gboolean (*lookup_by_cookie) (DBusGProxy *, const char *,
						GPtrArray **, GError **),
		  struct object * (*register_local) (DBusGProxy *,
						     GHashTable *,
						     const char *,
						     const char *,
						     const char *),
		  GError **caller_error)
{
  struct object *object = g_hash_table_lookup (hash, cookie);
  if (object)
    /* We already know about the object.  Return it.  */
    {
      assert (strcmp (object->cookie, cookie) == 0);
      return object;
    }

  /* Lookup the object.  */
  GPtrArray *objects = NULL;
  GError *error = NULL;
  if (! lookup_by_cookie (proxy, cookie, &objects, &error))
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return NULL;
    }

  if (objects->len > 1)
    /* There are multiple objects with the same cookie.  This is
       likely a programming bug.  Because it is not unambiguous which
       one is ours, there is little we can do.  To be on the safe
       side, we conservatively abort.  Recovering will require user
       intervention (remove a object), but that is better than
       corruption...  */
    {
      GString *s = g_string_new ("");

      int i;
      for (i = 0; i < objects->len; i ++)
	{
	  GValueArray *strct = g_ptr_array_index (objects, i);

	  GValue *a_human_readable_name_value
	    = g_value_array_get_nth (strct, 1);
	  const char *a_human_readable_name
	    = g_value_get_string (a_human_readable_name_value);
	  g_string_append_printf (s, "%s'%s'",
				  i > 0 ? ", " : "",
				  a_human_readable_name);
	}

      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_OBJECT_EXISTS,
		   "%s: Multiple objects with cookie '%s' exist (%s)."
		   "Aborting to avoid corruption.",
		   __FUNCTION__, cookie, s->str);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);

      g_string_free (s, TRUE);

      goto out;
    }

  if (objects->len == 0)
    /* A object with this cookie does not exist.  */
    goto out;

  GValueArray *strct = g_ptr_array_index (objects, 0);

  GValue *uuid_value = g_value_array_get_nth (strct, 0);
  const char *uuid = g_value_get_string (uuid_value);
  GValue *human_readable_name_value = g_value_array_get_nth (strct, 1);
  const char *human_readable_name
    = g_value_get_string (human_readable_name_value);

  object = register_local (proxy, hash, uuid, cookie, human_readable_name);

 out:
  if (objects)
    {
      int i;
      for (i = 0; i < objects->len; i ++)
	g_value_array_free (g_ptr_array_index (objects, i));
      g_ptr_array_free (objects, TRUE);
    }

  return object;
}
	       
static struct object *
stream_lookup (GWoodchuck *wc, const char *cookie, GError **caller_error)
{
  struct object *stream = ll_object_lookup
    (wc->manager_proxy, wc->stream_hash, cookie,
     org_woodchuck_manager_lookup_stream_by_cookie, stream_register_local,
     caller_error);

  if (stream)
    assert (stream->hash);

  return stream;
}

static struct object *
object_lookup (GWoodchuck *wc, struct object *stream,
	       const char *cookie, GError **caller_error)
{
  /* Better be a stream.  */
  assert (stream->hash);

  struct object *object = ll_object_lookup
    (stream->proxy, stream->hash, cookie,
     org_woodchuck_stream_lookup_object_by_cookie, object_register_local,
     caller_error);

  if (object)
    assert (! object->hash);

  return object;
}

static struct object *
object_find (GWoodchuck *wc, const char *stream_identifier,
	     const char *cookie, GError **caller_error)
{
  GError *error = NULL;
  struct object *stream = stream_lookup (wc, stream_identifier, &error);
  if (error)
    {
      g_critical ("%s: %s", __FUNCTION__, error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! stream)
    {
      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_NO_SUCH_OBJECT,
		   "%s: No stream with identifier '%s' exists.",
		   __FUNCTION__, stream_identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return NULL;
    }

  return object_lookup (wc, stream, cookie, caller_error);
}

static void
object_deregister_local (GWoodchuck *wc,
			 struct object *stream, struct object *object)
{
  assert (stream->hash);
  assert (! object->hash);

  if (! g_hash_table_remove (stream->hash, object->cookie))
    {
      g_error ("Failed to remove object %s from stream %s hash.",
	       object->cookie, stream->cookie);
      assert (0 == 1);
    }

  g_free (object);
}

static void
stream_deregister_local (GWoodchuck *wc, struct object *stream)
{
  assert (stream->hash);

  if (! g_hash_table_remove (wc->stream_hash, stream->cookie))
    {
      g_error ("Failed to remove stream %s from wc->stream_hash.",
	       stream->cookie);
      assert (0 == 1);
    }

  void iter (gpointer key, gpointer value, gpointer user_data)
  {
    struct object *object = value;
    assert (! object->hash);
    object_deregister_local (wc, stream, object);
  }
  g_hash_table_foreach (stream->hash, iter, NULL);

  g_free (stream);

  return;
}

gboolean
gwoodchuck_stream_register (GWoodchuck *wc,
			    const char *identifier,
			    const char *human_readable_name, uint32_t freshness,
			    GError **caller_error)
{
  /* We require that the identifier be unique.  */
  GError *error = NULL;
  struct object *stream = stream_lookup (wc, identifier, &error);
  if (error)
    {
      g_critical ("%s: %s", __FUNCTION__, error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (stream)
    {
      assert (strcmp (stream->cookie, identifier) == 0);

      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_OBJECT_EXISTS,
		   "%s: Register stream '%s': A stream ('%s') with "
		   "identifier '%s' already exists.",
		   __FUNCTION__,
		   human_readable_name, stream->human_readable_name,
		   identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);

      return FALSE;
    }

  /* The stream does not appear to exist.  Register it.  */

  GHashTable *properties = g_hash_table_new (g_str_hash, g_str_equal);

  void adds (char *key, const char *value, GValue *gvalue)
  {
    memset (gvalue, 0, sizeof (*gvalue));
    g_value_init (gvalue, G_TYPE_STRING);
    g_value_set_static_string (gvalue, value);
    g_hash_table_insert (properties, key, gvalue);
  }
  void addu (char *key, uint32_t value, GValue *gvalue)
  {
    memset (gvalue, 0, sizeof (*gvalue));
    g_value_init (gvalue, G_TYPE_UINT);
    g_value_set_uint (gvalue, value);
    g_hash_table_insert (properties, key, gvalue);
  }

  GValue human_readable_name_value;
  adds ("HumanReadableName", human_readable_name, &human_readable_name_value);
  GValue cookie_value;
  adds ("Cookie", identifier, &cookie_value);
  GValue freshness_value;
  addu ("Freshness", freshness, &freshness_value);

  char *uuid = NULL;
  gboolean ret = org_woodchuck_manager_stream_register (wc->manager_proxy,
							properties, TRUE,
							&uuid, &error);
  g_hash_table_unref (properties);
  if (! ret)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  stream = stream_register_local (wc->manager_proxy, wc->stream_hash,
				  uuid, identifier, human_readable_name);

  g_free (uuid);

  return stream != NULL;
}

gboolean
gwoodchuck_stream_updated_full (GWoodchuck *wc,
				const char *stream_identifier,
				uint32_t indicator_mask,
				uint64_t transferred_up,
				uint64_t transferred_down,
				uint64_t start,
				uint32_t duration,
				uint32_t new_objects,
				uint32_t updated_objects,
				uint32_t objects_inline,
				GError **caller_error)
{
  GError *error = NULL;
  struct object *stream = stream_lookup (wc, stream_identifier, &error);
  if (error)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! stream)
    {
      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_NO_SUCH_OBJECT,
		   "%s: Stream '%s' is not registered.",
		   __FUNCTION__, stream_identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);

      return FALSE;
    }

  gboolean ret = org_woodchuck_stream_update_status
      (stream->proxy, 0, indicator_mask, transferred_up, transferred_down,
       start, duration, new_objects, updated_objects,
       objects_inline, &error);

  if (! ret)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  return TRUE;
}

gboolean
gwoodchuck_stream_updated (GWoodchuck *wc,
			   const char *stream_identifier,
			   uint64_t transferred,
			   uint32_t duration,
			   uint32_t new_objects,
			   uint32_t updated_objects,
			   uint32_t objects_inline,
			   GError **error)
{
  return gwoodchuck_stream_updated_full (wc, stream_identifier, 0,
					 0, transferred,
					 time (NULL) - duration, duration,
					 new_objects, updated_objects,
					 objects_inline, error);
}

gboolean
gwoodchuck_stream_update_failed (GWoodchuck *wc,
				 const char *stream_identifier,
				 uint32_t reason,
				 uint32_t transferred,
				 GError **caller_error)
{
  GError *error = NULL;
  struct object *stream = stream_lookup (wc, stream_identifier, &error);
  if (error)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! stream)
    {
      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_NO_SUCH_OBJECT,
		   "%s: Stream '%s' is not registered.",
		   __FUNCTION__, stream_identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);

      return FALSE;
    }

  gboolean ret = org_woodchuck_stream_update_status
      (stream->proxy, reason, 0, 0, transferred,
       time (NULL), 0, 0, 0, 0, &error);
  if (! ret)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  return TRUE;
}

gboolean
gwoodchuck_stream_unregister (GWoodchuck *wc, const char *identifier,
			      GError **caller_error)
{
  GError *error = NULL;
  struct object *stream = stream_lookup (wc, identifier, &error);
  if (error)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! org_woodchuck_stream_unregister (stream->proxy, FALSE, &error))
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  stream_deregister_local (wc, stream);

  return TRUE;
}

gboolean
gwoodchuck_object_register (GWoodchuck *wc,
			    const char *stream_identifier,
			    const char *object_identifier,
			    const char *human_readable_name,
			    int64_t expected_size,
			    uint64_t expected_transfer_up,
			    uint64_t expected_transfer_down,
			    uint32_t transfer_frequency,
			    GError **caller_error)
{
  GError *error = NULL;
  struct object *stream = stream_lookup (wc, stream_identifier, &error);
  if (error)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! stream)
    {
      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_NO_SUCH_OBJECT,
		   "%s: Stream '%s' is not registered.",
		   __FUNCTION__, stream_identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
    }

  GHashTable *properties = g_hash_table_new (g_str_hash, g_str_equal);

  void adds (char *key, const char *value, GValue *gvalue)
  {
    memset (gvalue, 0, sizeof (*gvalue));
    g_value_init (gvalue, G_TYPE_STRING);
    g_value_set_static_string (gvalue, value);
    g_hash_table_insert (properties, key, gvalue);
  }
  void addu (char *key, uint32_t value, GValue *gvalue)
  {
    memset (gvalue, 0, sizeof (*gvalue));
    g_value_init (gvalue, G_TYPE_UINT);
    g_value_set_uint (gvalue, value);
    g_hash_table_insert (properties, key, gvalue);
  }

  GValue human_readable_name_value;
  adds ("HumanReadableName", human_readable_name, &human_readable_name_value);
  GValue cookie_value;
  adds ("Cookie", object_identifier, &cookie_value);
  GValue wakeup_value;
  addu ("Wakeup", TRUE, &wakeup_value);

  GValueArray *strct = g_value_array_new (4);

  GValue url_value = { 0 };
  g_value_init (&url_value, G_TYPE_STRING);
  g_value_set_static_string (&url_value, "");
  g_value_array_append (strct, &url_value);

  GValue expected_size_value = { 0 };
  g_value_init (&expected_size_value, G_TYPE_INT64);
  g_value_set_int64 (&expected_size_value, expected_size);
  g_value_array_append (strct, &expected_size_value);

  GValue expected_transfer_up_value = { 0 };
  g_value_init (&expected_transfer_up_value, G_TYPE_UINT64);
  g_value_set_uint64 (&expected_transfer_up_value, expected_transfer_up);
  g_value_array_append (strct, &expected_transfer_up_value);

  GValue expected_transfer_down_value = { 0 };
  g_value_init (&expected_transfer_down_value, G_TYPE_UINT64);
  g_value_set_uint64 (&expected_transfer_down_value, expected_transfer_down);
  g_value_array_append (strct, &expected_transfer_down_value);

  GValue utility_value = { 0 };
  g_value_init (&utility_value, G_TYPE_UINT);
  g_value_set_uint (&utility_value, 1);
  g_value_array_append (strct, &utility_value);

  GValue use_simple_transferer_value = { 0 };
  g_value_init (&use_simple_transferer_value, G_TYPE_BOOLEAN);
  g_value_set_boolean (&use_simple_transferer_value, FALSE);
  g_value_array_append (strct, &use_simple_transferer_value);

  GPtrArray *versions = g_ptr_array_new ();
  g_ptr_array_add (versions, strct);

  static GType asxttub;
  if (! asxttub)
    asxttub = dbus_g_type_get_collection
      ("GPtrArray",
       dbus_g_type_get_struct ("GValueArray",
			       G_TYPE_STRING, G_TYPE_INT64,
			       G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT,
			       G_TYPE_BOOLEAN, G_TYPE_INVALID));

  GValue versions_value = { 0 };
  g_value_init (&versions_value, asxttub);
  g_value_set_boxed (&versions_value, versions);

  g_hash_table_insert (properties, "Versions", &versions_value);

  char *uuid = NULL;
  gboolean ret = org_woodchuck_stream_object_register (stream->proxy,
						       properties, TRUE,
						       &uuid, &error);

  g_hash_table_unref (properties);
  g_value_array_free (strct);
  g_ptr_array_free (versions, TRUE);

  if (! ret)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  return TRUE;
}

gboolean
gwoodchuck_object_unregister (GWoodchuck *wc,
			      const char *stream_identifier,
			      const char *object_identifier,
			      GError **caller_error)
{
  GError *error = NULL;
  struct object *stream = stream_lookup (wc, stream_identifier, &error);
  if (error)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! stream)
    {
      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_NO_SUCH_OBJECT,
		   "%s: Stream '%s' is not registered.",
		   __FUNCTION__, stream_identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
    }

  struct object *object = object_lookup (wc, stream, object_identifier, &error);
  if (error)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! object)
    {
      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_NO_SUCH_OBJECT,
		   "%s: Object '%s' is not registered in stream '%s'.",
		   __FUNCTION__, object_identifier, stream_identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);

      return FALSE;
    }

  if (! org_woodchuck_object_unregister (object->proxy, &error))
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  object_deregister_local (wc, stream, object);

  return TRUE;
}

gboolean
gwoodchuck_object_transferred_full
  (GWoodchuck *wc, const char *stream_identifier, const char *object_identifier,
   uint32_t indicator_mask, uint64_t transferred_up, uint64_t transferred_down,
   uint64_t transfer_time, uint32_t transfer_duration, uint64_t object_size,
   struct gwoodchuck_object_transferred_file *files, int files_count,
   GError **caller_error)
{
  GError *error = NULL;
  struct object *object = object_find (wc, stream_identifier,
				       object_identifier, &error);
  if (error)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! object)
    {
      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_NO_SUCH_OBJECT,
		   "%s: Object '%s' is not registered in stream '%s'.",
		   __FUNCTION__, object_identifier, stream_identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);

      return FALSE;
    }

  GPtrArray *files_ptr_array = g_ptr_array_new ();
  int i;
  for (i = 0; i < files_count; i ++)
    {
      GValueArray *strct = g_value_array_new (3);

      GValue *filename_value = alloca (sizeof (GValue));
      memset (filename_value, 0, sizeof (GValue));
      g_value_init (filename_value, G_TYPE_STRING);
      g_value_set_static_string (filename_value, files[i].filename);
      g_value_array_append (strct, filename_value);

      GValue *dedicated_value = alloca (sizeof (GValue));
      memset (dedicated_value, 0, sizeof (GValue));
      g_value_init (dedicated_value, G_TYPE_BOOLEAN);
      g_value_set_boolean (dedicated_value, files[i].dedicated);
      g_value_array_append (strct, dedicated_value);

      GValue *deletion_policy_value = alloca (sizeof (GValue));
      memset (deletion_policy_value, 0, sizeof (GValue));
      g_value_init (deletion_policy_value, G_TYPE_UINT);
      g_value_set_uint (deletion_policy_value, files[i].deletion_policy);
      g_value_array_append (strct, deletion_policy_value);

      g_ptr_array_add (files_ptr_array, strct);
    }

  gboolean ret = org_woodchuck_object_transfer_status
      (object->proxy, 0, indicator_mask, transferred_up, transferred_down,
       transfer_time, transfer_duration, object_size, files_ptr_array,
       &error);

  for (i = 0; i < files_count; i ++)
    g_value_array_free (g_ptr_array_index (files_ptr_array, i));
  g_ptr_array_free (files_ptr_array, TRUE);

  if (! ret)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  return TRUE;
}

gboolean
gwoodchuck_object_transferred
  (GWoodchuck *wc, const char *stream_identifier, const char *object_identifier,
   uint32_t indicator_mask, uint64_t object_size,
   uint32_t transfer_duration, const char *filename,
   uint32_t deletion_policy, GError **error)
{
  struct gwoodchuck_object_transferred_file file =
    {
      filename, TRUE, deletion_policy
    };

  return gwoodchuck_object_transferred_full
    (wc, stream_identifier, object_identifier,
     indicator_mask, 0, object_size,
     time (NULL) - transfer_duration, transfer_duration, object_size,
     &file, 1, error);
}

gboolean
gwoodchuck_object_transfer_failed (GWoodchuck *wc,
				   const char *stream_identifier,
				   const char *object_identifier,
				   uint32_t reason,
				   uint32_t transferred,
				   GError **caller_error)
{
  GError *error = NULL;
  struct object *object = object_find (wc,stream_identifier, object_identifier,
				       &error);
  if (error)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! object)
    {
      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_NO_SUCH_OBJECT,
		   "%s: Object '%s' is not registered in stream '%s'.",
		   __FUNCTION__, object_identifier, stream_identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);

      return FALSE;
    }

  GPtrArray *files_ptr_array = g_ptr_array_new ();

  gboolean ret = org_woodchuck_object_transfer_status
      (object->proxy, reason, 0, 0, transferred,
       time (NULL), 0, 0, files_ptr_array, &error);
  g_ptr_array_free (files_ptr_array, TRUE);
  if (! ret)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  return TRUE;
}

gboolean
gwoodchuck_object_used_full (GWoodchuck *wc,
			     const char *stream_identifier,
			     const char *object_identifier,
			     uint64_t start, uint64_t duration,
			     uint64_t use_mask,
			     GError **caller_error)
{
  GError *error = NULL;
  struct object *object = object_find (wc, stream_identifier, object_identifier,
				       &error);
  if (error)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! object)
    {
      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_NO_SUCH_OBJECT,
		   "%s: Object '%s' is not registered in stream '%s'.",
		   __FUNCTION__, object_identifier, stream_identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);

      return FALSE;
    }

  if (! org_woodchuck_object_used (object->proxy, start, duration, use_mask,
				   &error))
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  return TRUE;
}

gboolean
gwoodchuck_object_used (GWoodchuck *wc,
			const char *stream_identifier,
			const char *object_identifier,
			GError **error)
{
  time_t now = time (NULL);
  return gwoodchuck_object_used_full (wc, stream_identifier, object_identifier,
				      now, now, -1, error);
}

gboolean
gwoodchuck_object_files_deleted (GWoodchuck *wc,
				 const char *stream_identifier,
				 const char *object_identifier,
				 enum woodchuck_deletion_response response,
				 uint64_t arg,
				 GError **caller_error)
{
  GError *error = NULL;
  struct object *object = object_find (wc, stream_identifier, object_identifier,
				       &error);
  if (error)
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  if (! object)
    {
      g_set_error (&error, G_WOODCHUCK_ERROR, WOODCHUCK_ERROR_NO_SUCH_OBJECT,
		   "%s: Object '%s' is not registered in stream '%s'.",
		   __FUNCTION__, object_identifier, stream_identifier);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);

      return FALSE;
    }

  if (! org_woodchuck_object_files_deleted
      (object->proxy, response, arg, &error))
    {
      g_prefix_error (&error, "%s: ", __FUNCTION__);
      g_critical ("%s", error->message);
      g_propagate_error (caller_error, error);
      return FALSE;
    }

  return TRUE;
}

/* Process upcalls.  */
static gboolean
org_woodchuck_upcall_object_transferred (GWoodchuck *wc,
					const char *manager_uuid,
					const char *manager_cookie,
					const char *stream_uuid,
					const char *stream_cookie,
					const char *object_uuid,
					const char *object_cookie,
					uint32_t status,
					uint32_t instance,
					GValueArray *version_strct,
					const char *filename,
					uint64_t object_size,
					uint64_t trigger_target,
					uint64_t trigger_fired,
					GError **error)
{
#if 0
  int version_index
    = g_value_get_uint (g_value_array_get_nth (version_strct, 0));
  const char *url
    = g_value_get_string (g_value_array_get_nth (version_strct, 1));
  int64_t expected_size
    = g_value_get_int64 (g_value_array_get_nth (version_strct, 2));
  unt64_t expected_transfer_up
    = g_value_get_uint64 (g_value_array_get_nth (version_strct, 3));
  unt64_t expected_transfer_down
    = g_value_get_uint64 (g_value_array_get_nth (version_strct, 4));
  uint32_t utility
    = g_value_get_uint (g_value_array_get_nth (version_strct, 5));
  gboolean use_simple_transferer
    = g_value_get_boolean (g_value_array_get_nth (version_strct, 6));
#endif

  return FALSE;
}

static gboolean
org_woodchuck_upcall_stream_update (GWoodchuck *wc,
				    const char *manager_uuid,
				    const char *manager_cookie,
				    const char *stream_uuid,
				    const char *stream_cookie,
				    GError **error)
{
  if (wc->vtable && wc->vtable->stream_update)
    return wc->vtable->stream_update (stream_cookie, wc->user_data);

  return FALSE;
}

static gboolean
org_woodchuck_upcall_object_transfer (GWoodchuck *wc,
				      const char *manager_uuid,
				      const char *manager_cookie,
				      const char *stream_uuid,
				      const char *stream_cookie,
				      const char *object_uuid,
				      const char *object_cookie,
				      GValueArray *version_strct,
				      const char *filename,
				      uint32_t quality,
				      GError **error)
{
#if 0
  int version_index
    = g_value_get_uint (g_value_array_get_nth (version_strct, 0));
  const char *url
    = g_value_get_string (g_value_array_get_nth (version_strct, 1));
  int64_t expected_size
    = g_value_get_int64 (g_value_array_get_nth (version_strct, 2));
  unt64_t expected_transfer_up
    = g_value_get_uint64 (g_value_array_get_nth (version_strct, 3));
  unt64_t expected_transfer_down
    = g_value_get_uint64 (g_value_array_get_nth (version_strct, 4));
  uint32_t utility
    = g_value_get_uint (g_value_array_get_nth (version_strct, 5));
  gboolean use_simple_transferer
    = g_value_get_boolean (g_value_array_get_nth (version_strct, 6));
#endif

  if (wc->vtable && wc->vtable->object_transfer)
    {
      uint32_t ret = wc->vtable->object_transfer (stream_cookie, object_cookie,
						  quality, wc->user_data);
      return TRUE;
    }

  return FALSE;
}

static gboolean
org_woodchuck_upcall_object_delete_files (GWoodchuck *wc,
					  const char *manager_uuid,
					  const char *manager_cookie,
					  const char *stream_uuid,
					  const char *stream_cookie,
					  const char *object_uuid,
					  const char *object_cookie,
					  GPtrArray *files,
					  GError **error)
{
  if (wc->vtable && wc->vtable->object_delete)
    {
      const char *filenames[files->len + 1];
      int i;
      for (i = 0; i < files->len; i ++)
	{
	  GValueArray *strct = g_ptr_array_index (files, i);
	  GValue *filename_value = g_value_array_get_nth (strct, 0);
	  filenames[i] = g_value_get_string (filename_value);
	}
      filenames[i] = NULL;

      int64_t ret = wc->vtable->object_delete (stream_cookie, object_cookie,
					       filenames, wc->user_data);
      if (ret == 0)
	gwoodchuck_object_files_deleted (wc, stream_cookie, object_cookie,
					 WOODCHUCK_DELETE_DELETED, 0, error);
      else if (ret > 0)
	gwoodchuck_object_files_deleted (wc, stream_cookie, object_cookie,
					 WOODCHUCK_DELETE_REFUSED, ret, error);
      else
	gwoodchuck_object_files_deleted (wc, stream_cookie, object_cookie,
					 WOODCHUCK_DELETE_COMPRESSED, -1 * ret,
					 error);

      return TRUE;
    }

  return FALSE;
}

#ifdef GWOODCHUCK_TEST
int
main (int argc, char *argv[])
{
  g_type_init ();

  GError *error = NULL;
  GWoodchuck *wc = gwoodchuck_new ("Test", "org.woodchuck.test",
				   NULL, NULL, &error);
  if (! wc)
    {
      fprintf (stderr, "gwoodchuck_new(): %s", error->message);
      g_error_free (error);
      error = NULL;
      return 1;
    }

  struct object
  {
    char *human_readable_name;
    char *identifier;
  };

  struct object bb_objects[] =
    { { "Foo", "http://boingboing.net/1.xml" },
      { "Bar", "http://boingboing.net/2.xml" },
      { "Bam", "http://boingboing.net/3.xml" },
    };
    
  struct object pdo_objects[] =
    { { "Xyzzy", "http://planet.debian.org/1.xml" },
      { "Bar", "http://planet.debian.org/2.xml" },
    };
    
  struct
  {
    char *human_readable_name;
    char *identifier;
    struct object *objects;
    int object_count;
  } streams[] =
      { { "BoingBoing.net", "http://feeds.boingboing.net/boingboing/iBag",
	  bb_objects, sizeof (bb_objects) / sizeof (bb_objects[0]) },
	{ "Planet Debian", "http://planet.debian.org/rss20.xml",
	  pdo_objects, sizeof (pdo_objects) / sizeof (pdo_objects[0]) },
      };
  int stream_count = sizeof (streams) / sizeof (streams[0]);

  int i;
  for (i = 0; i < stream_count; i ++)
    {
      if (! gwoodchuck_stream_register
	  (wc, streams[i].identifier, streams[i].human_readable_name,
	   24 * 60 * 60, &error))
	{
	  if (error->code == WOODCHUCK_ERROR_OBJECT_EXISTS)
	    {
	      g_error_free (error);
	      error = NULL;
	    }
	  else
	    {
	      fprintf (stderr, "gwoodchuck_stream_register(%s): %s\n",
		       streams[i].human_readable_name, error->message);
	      fprintf (stderr, "error code: %d\n", error->code);
	      g_error_free (error);
	      error = NULL;
	      return 1;
	    }
	}

      int j;
      for (j = 0; j < streams[i].object_count; j ++)
	{
	  if (! gwoodchuck_stream_updated (wc, streams[i].identifier,
					  8000, 5,
					  streams[i].object_count, 0, 0,
					  &error))
	    {
	      fprintf (stderr, "gwoodchuck_stream_updated(%s): %s",
		       streams[i].human_readable_name, error->message);
	      g_error_free (error);
	      error = NULL;
	      return 1;
	    }

	  if (! gwoodchuck_object_register
	      (wc, streams[i].identifier, streams[i].objects[j].identifier,
	       streams[i].objects[j].human_readable_name, 100 + j * 10, 0, 0, 0,
	       &error))
	    {
	      if (error->code == WOODCHUCK_ERROR_OBJECT_EXISTS)
		{
		  g_error_free (error);
		  error = NULL;
		}
	      else
		{
		  fprintf (stderr, "gwoodchuck_object_register(%s, %s): %s\n",
			   streams[i].human_readable_name,
			   streams[i].objects[j].human_readable_name,
			   error->message);
		  fprintf (stderr, "error code: %d\n", error->code);
		  g_error_free (error);
		  error = NULL;
		  return 1;
		}
	    }

	  struct gwoodchuck_object_transferred_file file =
	    {
	      "/tmp/foo",
	      TRUE,
	      WOODCHUCK_DELETION_POLICY_DELETE_WITH_CONSULTATION
	    };
	  
	  int k;
	  for (k = 0; k < 3; k ++)
	    {
	      if (! gwoodchuck_object_transferred_full
		  (wc, streams[i].identifier, streams[i].objects[j].identifier,
		   0, 100, 200 + j * 10,
		   time (NULL), j,
		   100 + j * 10,
		   &file, 1, &error))
		{
		  fprintf (stderr, "gwoodchuck_object_transferred(%s, %s): %s",
			   streams[i].human_readable_name,
			   streams[i].objects[j].human_readable_name,
			   error->message);
		  g_error_free (error);
		  error = NULL;
		  return 1;
		}

	      time_t now = time (NULL);
	      if (! gwoodchuck_object_used_full
		  (wc, streams[i].identifier, streams[i].objects[j].identifier,
		   now, now, -1, &error))
		{
		  fprintf (stderr, "gwoodchuck_object_used(%s, %s): %s",
			   streams[i].human_readable_name,
			   streams[i].objects[j].human_readable_name,
			   error->message);
		  g_error_free (error);
		  error = NULL;
		  return 1;
		}
	    }

	  if (! gwoodchuck_object_transfer_failed
	      (wc, streams[i].identifier, streams[i].objects[j].identifier,
	       WOODCHUCK_TRANSFER_TRANSIENT_NETWORK, 100, &error))
	    {
	      fprintf (stderr, "gwoodchuck_object_transfer_failed(%s, %s): %s",
		       streams[i].human_readable_name,
		       streams[i].objects[j].human_readable_name,
		       error->message);
	      g_error_free (error);
	      error = NULL;
	      return 1;
	    }

	  if (! gwoodchuck_object_files_deleted
	      (wc, streams[i].identifier, streams[i].objects[j].identifier,
	       WOODCHUCK_DELETE_DELETED, 0, &error))
	    {
	      fprintf (stderr, "gwoodchuck_object_fails_deleted(%s, %s): %s",
		       streams[i].human_readable_name,
		       streams[i].objects[j].human_readable_name,
		       error->message);
	      g_error_free (error);
	      error = NULL;
	      return 1;
	    }

	  /* For I == 0, test removal of a stream with objects.  */
	  if (i != 0)
	    if (! gwoodchuck_object_unregister
		(wc, streams[i].identifier, streams[i].objects[j].identifier,
		 &error))
	      {
		fprintf (stderr, "gwoodchuck_object_unregister(%s, %s): %s",
			 streams[i].human_readable_name,
			 streams[i].objects[j].human_readable_name,
			 error->message);
		g_error_free (error);
		error = NULL;
		return 1;
	      }
	}

      if (! gwoodchuck_stream_update_failed
	  (wc, streams[i].identifier,
	   WOODCHUCK_TRANSFER_TRANSIENT_NETWORK, 200, &error))
	{
	  fprintf (stderr, "gwoodchuck_stream_update_failed(%s): %s",
		   streams[i].human_readable_name, error->message);
	  g_error_free (error);
	  error = NULL;
	  return 1;
	}


      if (! gwoodchuck_stream_unregister (wc, streams[i].identifier, &error))
	{
	  fprintf (stderr, "gwoodchuck_stream_unregister(%s): %s",
		   streams[i].human_readable_name, error->message);
	  g_error_free (error);
	  error = NULL;
	  return 1;
	}
    }

  return 0;
}
#endif
