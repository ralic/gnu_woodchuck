#ifndef NETCZAR_DBUS_UTIL_H
#define NETCZAR_DBUS_UTIL_H

#include <dbus/dbus.h>
#include <glib-object.h>
#include "org.freedesktop.DBus.Properties.h"

/* The type of property changes.  */
#define DBUS_TYPE_G_MAP_OF_VARIANT \
  (dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE))

static inline bool
dbus_property_lookup (DBusGProxy *object,
		      const char *path,
		      const char *interface, const char *property,
		      int type, GValue *ret)
{
  bool succ = false;

  DBusGProxy *proxy;

  proxy = dbus_g_proxy_new_from_proxy (object,
				       /* Interface.  */
				       "org.freedesktop.DBus.Properties",
				       /* Path.  */
				       path ?: dbus_g_proxy_get_path (object));

  GValue value = { 0 };
  GError *error = NULL;
  if (! org_freedesktop_DBus_Properties_get
          (proxy,
	   interface ?: dbus_g_proxy_get_interface (object), property,
	   &value, &error))
    {
      printf ("Getting %s.%s: %s\n",
	      interface, property, error->message);
      g_error_free (error);
      goto out;
    }

  g_value_init (ret, type);
  if (G_VALUE_TYPE (&value) == DBUS_TYPE_G_OBJECT_PATH)
    /* Arg, there are no transforms set up for DBUS_TYPE_G_OBJECT_PATH
       so we have to do this manually.  */
    {
      if (type == G_TYPE_STRING)
	g_value_set_string (ret, g_value_get_boxed (&value));
      else
	{
	  printf ("can't convert an object path to a %s\n",
		  g_type_name (type));
	  goto out;
	}
    }
  else if (G_VALUE_TYPE (&value) == DBUS_TYPE_G_UCHAR_ARRAY)
    /* See above, same problem.  */
    {
      if (type == G_TYPE_STRING)
	{
	  GArray *a = g_value_get_boxed (&value);
	  g_value_take_string (ret, g_strdup_printf ("%.*s", a->len, a->data));
	}
      else
	{
	  printf ("can't convert a uchar array to a %s\n",
		  g_type_name (type));
	  goto out;
	}
    }
  else if (G_VALUE_TYPE (&value) == type)
    g_value_copy (&value, ret);
  else if (! g_value_transform (&value, ret))
    {
      char *lit = g_strdup_value_contents (&value);
      printf ("Transforming %s.%s = %s from %s (parent: %s) to %s failed\n",
	      interface, property, lit,
	      G_VALUE_TYPE_NAME (&value),
	      g_type_name (g_type_parent (G_VALUE_TYPE (&value))),
	      g_type_name (type));
      g_free (lit);
      g_value_unset (ret);
      goto out;
    }

  succ = true;
 out:
  g_value_unset (&value);
  g_object_unref (proxy);
  return succ;
}

static inline char *
dbus_property_lookup_str (DBusGProxy *object,
			  const char *path,
			  const char *interface, const char *property)
{
  GValue value = { 0 };
  if (! dbus_property_lookup (object, path, interface, property,
			      G_TYPE_STRING, &value))
    return NULL;

  char *ret = g_value_dup_string (&value);
  g_value_unset (&value);
  return ret;
}

static inline int
dbus_property_lookup_int (DBusGProxy *object,
			  const char *path,
			  const char *interface, const char *property,
			  int def)
{
  GValue value = { 0 };
  if (! dbus_property_lookup (object, path, interface, property,
			      G_TYPE_INT, &value))
    return def;

  int ret = g_value_get_int (&value);
  g_value_unset (&value);
  return ret;
}
 
#endif
