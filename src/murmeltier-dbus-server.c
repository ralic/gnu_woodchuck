/* murmeltier-dbus-server.c - The dbus server details.
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

#include <assert.h>
#include <stdio.h>
#include <error.h>
#include <string.h>
#include <stdbool.h>
#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>

#include "debug.h"
#include "util.h"

#include "murmeltier-dbus-server.h"

#include "org.woodchuck.xml.h"
#include "org.woodchuck.manager.xml.h"
#include "org.woodchuck.stream.xml.h"
#include "org.woodchuck.object.xml.h"
#include "org.freedesktop.DBus.Introspectable.xml.h"
#include "org.freedesktop.DBus.Properties.xml.h"

static DBusHandlerResult
process_message (DBusConnection *connection, DBusMessage *message,
		 gpointer user_data)
{
  DBusMessage *reply = dbus_message_new_method_return (message);
  const char *error_name = NULL;
  /* The error message in ERROR is preferred to ERROR_MESSAGE.  */
  GError *error = NULL;
  char *error_message = NULL;
  enum woodchuck_error ret = WOODCHUCK_ERROR_GENERIC;
  char *expected_sig = NULL;
  const char *actual_sig = dbus_message_get_signature (message);

  const char *path = dbus_message_get_path (message);
  const char *method = dbus_message_get_member (message);
  const char *interface_str = dbus_message_get_interface (message);

  /* Particularly when dealing with properties, we have an array of
     structs.  A struct consists of a number of GValues.  We allocate
     these out of stack memory (alloca).  The struct itself is a
     GValueArray.  And the array is a GPtrArray of GValueArrays.  Any
     such GPtrArrays added to this list will be freed before returning
     from this function.  */
  GSList *array_of_structs_to_free = NULL;

  debug (5, "Invocation of %s.%s on %s", interface_str, method, path);

  enum
  {
    org_woodchuck = 1,
    org_woodchuck_manager,
    org_woodchuck_stream,
    org_woodchuck_object,
    org_freedesktop_dbus_introspectable,
    org_freedesktop_dbus_properties
  };
  int interface = 0;
  if (strcmp (interface_str, "org.woodchuck") == 0)
    interface = org_woodchuck;
  if (strcmp (interface_str, "org.woodchuck.manager") == 0)
    interface = org_woodchuck_manager;
  if (strcmp (interface_str, "org.woodchuck.stream") == 0)
    interface = org_woodchuck_stream;
  if (strcmp (interface_str, "org.woodchuck.object") == 0)
    interface = org_woodchuck_object;
  if (strcmp (interface_str, "org.freedesktop.DBus.Introspectable") == 0)
    interface = org_freedesktop_dbus_introspectable;
  if (strcmp (interface_str, "org.freedesktop.DBus.Properties") == 0)
    interface = org_freedesktop_dbus_properties;


#define PATH_ROOT "/org/woodchuck"
  if (strncmp (path, PATH_ROOT, sizeof (PATH_ROOT) - 1) != 0)
    /* Not for us.  */
    return DBUS_HANDLER_RESULT_HANDLED;

  path = &path[sizeof (PATH_ROOT) - 1];

  debug (5, "Path -> '%s'", path);

#define PATH_MANAGER "manager/"
#define PATH_STREAM "stream/"
#define PATH_OBJECT "object/"
  enum
  {
    root = 1, manager, stream, object
  };
  int type = 0;

  if (*path == '\0')
    {
      debug (5, "Object type: root");
      type = root;
      if (! (interface == org_freedesktop_dbus_introspectable
	     || interface == org_freedesktop_dbus_properties
	     || interface == org_woodchuck))
	interface = 0;
    }
  else if (*path == '/')
    {
      path ++;
      if (strncmp (path, PATH_MANAGER, sizeof (PATH_MANAGER) - 1) == 0)
	{
	  debug (5, "Object type: manager");
	  path += sizeof (PATH_MANAGER) - 1;
	  type = manager;
	  if (! (interface == org_freedesktop_dbus_introspectable
		 || interface == org_freedesktop_dbus_properties
		 || interface == org_woodchuck_manager))
	    interface = 0;
	}
      else if (strncmp (path, PATH_STREAM, sizeof (PATH_STREAM) - 1) == 0)
	{
	  debug (5, "Object type: stream");
	  path += sizeof (PATH_STREAM) - 1;
	  type = stream;
	  if (! (interface == org_freedesktop_dbus_introspectable
		 || interface == org_freedesktop_dbus_properties
		 || interface == org_woodchuck_stream))
	    interface = 0;
	}
      if (strncmp (path, PATH_OBJECT, sizeof (PATH_OBJECT) - 1) == 0)
	{
	  debug (5, "Object type: object");
	  path += sizeof (PATH_OBJECT) - 1;
	  type = object;
	  if (! (interface == org_freedesktop_dbus_introspectable
		 || interface == org_freedesktop_dbus_properties
		 || interface == org_woodchuck_object))
	    interface = 0;
	}
    }

  int hexdigits = strspn (path, "0123456789abcdef");
  if (! type || path[hexdigits] != '\0')
    /* Bad object name.  */
    {
      debug (3, "Bad object name: %s.", path);
      error = g_error_new (DBUS_GERROR, 0, "%s: No such object.",
			   dbus_message_get_path (message));
      error_name = DBUS_ERROR_UNKNOWN_OBJECT;
      goto out;
    }

  if (! interface)
    {
      error_name = DBUS_ERROR_UNKNOWN_INTERFACE;
      goto bad_method;
    }

  debug (5, "Object is '%s'", path);

  /* Demux and demarshal.  */
  if (interface == org_freedesktop_dbus_introspectable
      && strcmp (method, "Introspect") == 0)
    {
      expected_sig = "";
      if (strcmp (expected_sig, actual_sig) != 0)
	goto bad_signature;

#define XML_PREFIX \
      "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n" \
      "<node>\n"
#define XML_POSTFIX "</node>\n"
      const char *xml = NULL;
      switch (type)
	{
	case root:
	  xml = XML_PREFIX \
	    ORG_WOODCHUCK_XML \
	    ORG_FREEDESKTOP_DBUS_INTROSPECTABLE_XML \
	    ORG_FREEDESKTOP_DBUS_PROPERTIES_XML \
	    XML_POSTFIX;
	  break;
	case manager:
	  xml = XML_PREFIX \
	    ORG_WOODCHUCK_MANAGER_XML \
	    ORG_FREEDESKTOP_DBUS_INTROSPECTABLE_XML \
	    ORG_FREEDESKTOP_DBUS_PROPERTIES_XML \
	    XML_POSTFIX;
	  break;
	case stream:
	  xml = XML_PREFIX \
	    ORG_WOODCHUCK_STREAM_XML \
	    ORG_FREEDESKTOP_DBUS_INTROSPECTABLE_XML \
	    ORG_FREEDESKTOP_DBUS_PROPERTIES_XML \
	    XML_POSTFIX;
	  break;
	case object:
	  xml = XML_PREFIX \
	    ORG_WOODCHUCK_OBJECT_XML \
	    ORG_FREEDESKTOP_DBUS_INTROSPECTABLE_XML \
	    ORG_FREEDESKTOP_DBUS_PROPERTIES_XML \
	    XML_POSTFIX;
	  break;
	}

      dbus_message_append_args (reply, DBUS_TYPE_STRING, &xml,
				DBUS_TYPE_INVALID);
      ret = 0;
    }
  else if (interface == org_freedesktop_dbus_properties
	   && strcmp (method, "Get") == 0)
    {
      const char *interface_name = NULL;
      const char *property_name = NULL;

      expected_sig = "ss";
      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if (strcmp (expected_sig, actual_sig) != 0
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_STRING, &interface_name,
				      DBUS_TYPE_STRING, &property_name,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      GValue value = { 0 };
      if (type == root)
	ret = woodchuck_property_get (path, interface_name,
				      property_name, &value, &error);
      if (type == manager)
	ret = woodchuck_manager_property_get (path, interface_name,
					      property_name, &value, &error);
      if (type == stream)
	ret = woodchuck_stream_property_get (path, interface_name,
					     property_name, &value, &error);
      if (type == object)
	ret = woodchuck_object_property_get (path, interface_name,
					     property_name, &value, &error);

      if (ret == 0)
	{
	  DBusMessageIter outer_iter;
	  dbus_message_iter_init_append (reply, &outer_iter);

	  void add (char dtype, void *value)
	  {
	    char dtypestr[2] = { dtype, '\0' };

	    DBusMessageIter variant_iter;
	    dbus_message_iter_open_container (&outer_iter,
					      DBUS_TYPE_VARIANT,
					      dtypestr,
					      &variant_iter);

	    dbus_message_iter_append_basic (&variant_iter, dtype, value);

	    dbus_message_iter_close_container (&outer_iter, &variant_iter);
	  }

	  switch (G_VALUE_TYPE (&value))
	    {
	    case G_TYPE_INT:
	      {
		dbus_int32_t v = g_value_get_int (&value);
		add (DBUS_TYPE_INT32, &v);
		break;
	      }
	    case G_TYPE_UINT:
	      {
		dbus_uint32_t v = g_value_get_uint (&value);
		add (DBUS_TYPE_UINT32, &v);
		break;
	      }
	    case G_TYPE_INT64:
	      {
		dbus_int64_t v = g_value_get_int64 (&value);
		add (DBUS_TYPE_INT64, &v);
		break;
	      }
	    case G_TYPE_UINT64:
	      {
		dbus_uint64_t v = g_value_get_uint64 (&value);
		add (DBUS_TYPE_UINT64, &v);
		break;
	      }
	    case G_TYPE_BOOLEAN:
	      {
		dbus_bool_t v = g_value_get_boolean (&value);
		add (DBUS_TYPE_BOOLEAN, &v);
		break;
	      }
	    case G_TYPE_STRING:
	      {
		const char *v = g_value_get_string (&value);
		add (DBUS_TYPE_STRING, &v);
		break;
	      }
	    default:
	      error_message = g_strdup_printf
		("Cannot return property: unsupported type.");
	      goto bad_signature;
	    }
	}

      if (G_IS_VALUE (&value))
	g_value_unset (&value);
    }
  else if (interface == org_freedesktop_dbus_properties
	   && strcmp (method, "Set") == 0)
    {
      const char *interface_name = NULL;
      const char *property_name = NULL;

      expected_sig = "ssv";
      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if ((strcmp (expected_sig, actual_sig) != 0
	   /* Make the variant optional so that it is possible to set
	      properties using dbus-send.  */
	   && !(strlen (actual_sig) == 3
		&& strncmp (expected_sig, actual_sig, 2) == 0))
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_STRING, &interface_name,
				      DBUS_TYPE_STRING, &property_name,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      GValue value = { 0 };

      DBusMessageIter outer_iter;
      dbus_message_iter_init (message, &outer_iter);
      /* Skip interface_name and property_name.  */
      dbus_message_iter_next (&outer_iter);
      dbus_message_iter_next (&outer_iter);

      DBusMessageIter variant_iter;
      DBusMessageIter *iter = &outer_iter;
      if (actual_sig[2] == 'v')
	{
	  dbus_message_iter_recurse (&outer_iter, &variant_iter);
	  iter = &variant_iter;
	}

      int arg_type = dbus_message_iter_get_arg_type (iter);
      switch (arg_type)
	{
	case DBUS_TYPE_INT32:
	  {
	    dbus_int32_t v;
	    dbus_message_iter_get_basic (iter, &v);
	    g_value_init (&value, G_TYPE_INT);
	    g_value_set_int (&value, v);
	    break;
	  }
	case DBUS_TYPE_UINT32:
	  {
	    dbus_uint32_t v;
	    dbus_message_iter_get_basic (iter, &v);
	    g_value_init (&value, G_TYPE_UINT);
	    g_value_set_uint (&value, v);
	    break;
	  }
	case DBUS_TYPE_INT64:
	  {
	    dbus_int64_t v;
	    dbus_message_iter_get_basic (iter, &v);
	    g_value_init (&value, G_TYPE_INT64);
	    g_value_set_int64 (&value, v);
	    break;
	  }
	case DBUS_TYPE_UINT64:
	  {
	    dbus_uint64_t v;
	    dbus_message_iter_get_basic (iter, &v);
	    g_value_init (&value, G_TYPE_UINT64);
	    g_value_set_uint64 (&value, v);
	    break;
	  }
	case DBUS_TYPE_BOOLEAN:
	  {
	    dbus_bool_t v;
	    dbus_message_iter_get_basic (iter, &v);
	    g_value_init (&value, G_TYPE_BOOLEAN);
	    g_value_set_boolean (&value, v);
	    break;
	  }
	case DBUS_TYPE_STRING:
	  {
	    char *v;
	    dbus_message_iter_get_basic (iter, &v);
	    g_value_init (&value, G_TYPE_STRING);
	    g_value_set_string (&value, v);
	    break;
	  }
	default:
	  error_message = g_strdup_printf
	    ("Cannot set property: unsupported type.");
	  goto bad_signature;
	}

      if (type == root)
	ret = woodchuck_property_set (path, interface_name,
				      property_name, &value, &error);
      if (type == manager)
	ret = woodchuck_manager_property_set (path, interface_name,
					      property_name, &value, &error);
      if (type == stream)
	ret = woodchuck_stream_property_set (path, interface_name,
					     property_name, &value, &error);
      if (type == object)
	ret = woodchuck_object_property_set (path, interface_name,
					     property_name, &value, &error);

      if (G_IS_VALUE (&value))
	g_value_unset (&value);
    }
  else if ((type == root && strcmp (method, "ManagerRegister") == 0)
	   || (type == manager && strcmp (method, "ManagerRegister") == 0)
	   || (type == manager && strcmp (method, "StreamRegister") == 0)
	   || (type == stream && strcmp (method, "ObjectRegister") == 0))
    /* Single argument: a property dictionary.  */
    {
      /* In reality, we accept either a{sv}b or a{ss}b.  The main reason
	 is that dbus-send cannot send messages with a variant
	 type.  */
      expected_sig = "a{sv}b";

      GHashTable *properties = g_hash_table_new (g_str_hash, g_str_equal);
      GValue *values = NULL;
      gboolean only_if_unique = FALSE;

      DBusMessageIter outer_iter;
      dbus_message_iter_init (message, &outer_iter);
      int arg_type = dbus_message_iter_get_arg_type (&outer_iter);

      if (arg_type == DBUS_TYPE_ARRAY)
	/* The dictionary is optional.  */
	{
	  DBusMessageIter array_iter;
	  dbus_message_iter_recurse (&outer_iter, &array_iter);

	  int array_count = 0;
	  while (dbus_message_iter_get_arg_type (&array_iter)
		 != DBUS_TYPE_INVALID)
	    {
	      array_count ++;
	      dbus_message_iter_next (&array_iter);
	    }

	  values = g_malloc0 (sizeof (values[0]) * array_count);
	  
	  int i = 0;
	  dbus_message_iter_recurse (&outer_iter, &array_iter);
	  while ((arg_type = dbus_message_iter_get_arg_type (&array_iter))
		 != DBUS_TYPE_INVALID)
	    {
	      if (arg_type != DBUS_TYPE_DICT_ENTRY)
		goto register_bad_type;

	      DBusMessageIter dict_entry_iter;
	      dbus_message_iter_recurse (&array_iter, &dict_entry_iter);

	      if ((arg_type = dbus_message_iter_get_arg_type (&dict_entry_iter))
		  != DBUS_TYPE_STRING)
		goto register_bad_type;

	      char *key = NULL;
	      dbus_message_iter_get_basic (&dict_entry_iter, &key);
	      debug (5, "Dict entry key: %s", key);

	      dbus_message_iter_next (&dict_entry_iter);
	      arg_type = dbus_message_iter_get_arg_type (&dict_entry_iter);

	      if (arg_type == DBUS_TYPE_VARIANT)
		{
		  DBusMessageIter variant_iter;
		  dbus_message_iter_recurse (&dict_entry_iter, &variant_iter);

		  arg_type = dbus_message_iter_get_arg_type (&variant_iter);
		  debug (5, "Key %s's value has type '%c'", key, arg_type);
		  switch (arg_type)
		    {
		    case DBUS_TYPE_STRING:
		      {
			char *value = NULL;
			dbus_message_iter_get_basic (&variant_iter, &value);
			debug (5, "Dict entry value: %s", value);

			g_value_init (&values[i], G_TYPE_STRING);
			g_value_set_static_string (&values[i], value);
			break;
		      }
		    case DBUS_TYPE_UINT32:
		      {
			uint32_t value = 10011001;
			dbus_message_iter_get_basic (&variant_iter, &value);
			debug (5, "Dict entry value: %d", value);

			g_value_init (&values[i], G_TYPE_UINT);
			g_value_set_uint (&values[i], value);
			break;
		      }
		    case DBUS_TYPE_UINT64:
		      {
			uint64_t value = 10011001;
			dbus_message_iter_get_basic (&variant_iter, &value);
			debug (5, "Dict entry value: %"PRId64, value);

			g_value_init (&values[i], G_TYPE_UINT64);
			g_value_set_uint64 (&values[i], value);
			break;
		      }
		    case DBUS_TYPE_ARRAY:
		      {
			if (strcmp ("a(sxttub)",
				    dbus_message_iter_get_signature
				    (&variant_iter)) != 0)
			  goto register_bad_type;

			DBusMessageIter array_iter;
			dbus_message_iter_recurse (&variant_iter, &array_iter);

			int array_len = 0;
			while (dbus_message_iter_get_arg_type (&array_iter)
			       != DBUS_TYPE_INVALID)
			  {
			    array_len ++;
			    dbus_message_iter_next (&array_iter);
			  }

			GPtrArray *array = g_ptr_array_new ();
			/* Don't forget to free this.  */
			array_of_structs_to_free = g_slist_prepend
			  (array_of_structs_to_free, array);

			int struct_len = 0;
			dbus_message_iter_recurse (&variant_iter, &array_iter);
			DBusMessageIter struct_iter;
			dbus_message_iter_recurse (&array_iter, &struct_iter);
			while (dbus_message_iter_get_arg_type (&struct_iter)
			       != DBUS_TYPE_INVALID)
			  {
			    struct_len ++;
			    dbus_message_iter_next (&struct_iter);
			  }
			GType types[struct_len];

			int j = 0;
			dbus_message_iter_recurse (&variant_iter, &array_iter);
			while ((arg_type
				= dbus_message_iter_get_arg_type (&array_iter))
			       != DBUS_TYPE_INVALID)
			  {
			    GValueArray *strct = g_value_array_new (4);

			    DBusMessageIter struct_iter;
			    dbus_message_iter_recurse (&array_iter,
						       &struct_iter);

			    int k = 0;
			    int element_type;
			    while ((element_type
				    = dbus_message_iter_get_arg_type
				    (&struct_iter))
				   != DBUS_TYPE_INVALID)
			      {
				GValue *value = alloca (sizeof (*value));
				memset (value, 0, sizeof (*value));

				GType gtype;
				switch (element_type)
				  {
				  case DBUS_TYPE_STRING:
				    {
				      gtype = G_TYPE_STRING;

				      char *s = NULL;
				      dbus_message_iter_get_basic (&struct_iter,
								   &s);
				      g_value_init (value, gtype);
				      g_value_set_static_string (value, s);
				      break;
				    }
				  case DBUS_TYPE_UINT32:
				    {
				      gtype = G_TYPE_UINT;

				      uint32_t s = 0;
				      dbus_message_iter_get_basic (&struct_iter,
								   &s);
				      g_value_init (value, gtype);
				      g_value_set_uint (value, s);
				      break;
				    }
				  case DBUS_TYPE_UINT64:
				    {
				      gtype = G_TYPE_UINT64;

				      uint64_t s = 0;
				      dbus_message_iter_get_basic (&struct_iter,
								   &s);
				      g_value_init (value, gtype);
				      g_value_set_uint64 (value, s);
				      break;
				    }
				  case DBUS_TYPE_INT64:
				    {
				      gtype = G_TYPE_INT64;

				      uint64_t s = 0;
				      dbus_message_iter_get_basic (&struct_iter,
								   &s);
				      g_value_init (value, gtype);
				      g_value_set_int64 (value, s);
				      break;
				    }
				  case DBUS_TYPE_BOOLEAN:
				    {
				      gtype = G_TYPE_BOOLEAN;

				      gboolean s = 0;
				      dbus_message_iter_get_basic (&struct_iter,
								   &s);
				      g_value_init (value, gtype);
				      g_value_set_boolean (value, s);
				      break;
				    }
				  default:
				    {
				      debug (0, "Bad array element type: %c",
					     element_type);
				      goto register_bad_type;
				    }
				  }

				if (j == 0)
				  types[k] = gtype;
				else if (types[k] != gtype)
				  goto register_bad_type;

				g_value_array_append (strct, value);
				k ++;
				if (k > struct_len)
				  goto register_bad_type;
				dbus_message_iter_next (&struct_iter);
			      }

			    g_ptr_array_add (array, strct);
			    j ++;
			    dbus_message_iter_next (&array_iter);
			  }

			GType strct_type = dbus_g_type_get_structv
			  ("GValueArray", struct_len, types);
			GType array_type
			  = dbus_g_type_get_collection
			  ("GPtrArray", strct_type);

			g_value_init (&values[i], array_type);
			g_value_set_boxed (&values[i], array);

			break;
		      }
		    default:
		      goto register_bad_type;
		    }
		}
	      else if (arg_type == DBUS_TYPE_STRING)
		{
		  char *value = NULL;
		  dbus_message_iter_get_basic (&dict_entry_iter, &value);
		  debug (5, "Dict entry value: %s", value);

		  g_value_init (&values[i], G_TYPE_STRING);
		  g_value_set_static_string (&values[i], value);
		}
	      else
		{
		  error_message = g_strdup_printf
		    ("Property %s has unsupported type %c",
		     key, arg_type);
		  goto register_bad_type;
		}

	      g_hash_table_insert (properties, key, &values[i]);

	      dbus_message_iter_next (&dict_entry_iter);
	      if ((arg_type = dbus_message_iter_get_arg_type (&dict_entry_iter))
		  != DBUS_TYPE_INVALID)
		goto register_bad_type;

	      i ++;
	      dbus_message_iter_next (&array_iter);
	    }

	  dbus_message_iter_next (&outer_iter);
	  arg_type = dbus_message_iter_get_arg_type (&outer_iter);
	}

      if (arg_type != DBUS_TYPE_BOOLEAN)
	goto register_bad_type;

      dbus_message_iter_get_basic (&outer_iter, &only_if_unique);
      dbus_message_iter_next (&outer_iter);
      arg_type = dbus_message_iter_get_arg_type (&outer_iter);

      if (arg_type != DBUS_TYPE_INVALID)
	goto register_bad_type;

      char *uuid = NULL;
      if (type == root && strcmp (method, "ManagerRegister") == 0)
	ret = woodchuck_manager_register (properties, only_if_unique,
					  &uuid, &error);
      else if (type == manager && strcmp (method, "ManagerRegister") == 0)
	ret = woodchuck_manager_manager_register (path,
						  properties, only_if_unique,
						  &uuid, &error);
      else if (type == manager && strcmp (method, "StreamRegister") == 0)
	ret = woodchuck_manager_stream_register (path,
						 properties, only_if_unique,
						 &uuid, &error);
      else if (type == stream && strcmp (method, "ObjectRegister") == 0)
	ret = woodchuck_stream_object_register (path,
						properties, only_if_unique,
						&uuid, &error);

      if (ret == 0)
	dbus_message_append_args (reply, DBUS_TYPE_STRING, &uuid,
				  DBUS_TYPE_INVALID);

      g_free (uuid);

      g_hash_table_unref (properties);
      g_free (values);

      if (0)
	{
	register_bad_type:
	  g_hash_table_unref (properties);
	  g_free (values);
	  goto bad_signature;
	}
    }
  else if (/* In: boolean.  */
	   (type == root && strcmp (method, "ListManagers") == 0)
	   /* In: string, boolean.  */
	   || (type == root && strcmp (method, "LookupManagerByCookie") == 0)
	   /* In: boolean.  */
	   || (type == manager && strcmp (method, "ListManagers") == 0)
	   /* In: string, boolean.  */
	   || (type == manager && strcmp (method, "LookupManagerByCookie") == 0)
	   /* In: none.  */
	   || (type == manager && strcmp (method, "ListStreams") == 0)
	   /* In: string.  */
	   || (type == manager && strcmp (method, "LookupStreamByCookie") == 0)
	   /* In: none.  */
	   || (type == stream && strcmp (method, "ListObjects") == 0)
	   /* In: string.  */
	   || (type == stream && strcmp (method, "LookupObjectByCookie") == 0))
    {
      DBusMessageIter iter;
      dbus_message_iter_init (message, &iter);
      int arg_type = dbus_message_iter_get_arg_type (&iter);

      char *cookie = NULL;
      bool recurse = true;

      if (strcmp (method, "LookupManagerByCookie") == 0)
	/* In: string, boolean.  */
	{
	  expected_sig = "sb";
	  if (arg_type != DBUS_TYPE_STRING)
	    goto bad_signature;

	  dbus_message_iter_get_basic (&iter, &cookie);

	  dbus_message_iter_next (&iter);
	  arg_type = dbus_message_iter_get_arg_type (&iter);

	  if (arg_type != DBUS_TYPE_BOOLEAN)
	    goto bad_signature;

	  dbus_message_iter_get_basic (&iter, &recurse);

	  dbus_message_iter_next (&iter);
	  arg_type = dbus_message_iter_get_arg_type (&iter);
	}
      else if (strncmp (method, "Lookup", 6) == 0)
	/* In: string.  */
	{
	  expected_sig = "s";
	  if (arg_type != DBUS_TYPE_STRING)
	    goto bad_signature;

	  dbus_message_iter_get_basic (&iter, &cookie);

	  dbus_message_iter_next (&iter);
	  arg_type = dbus_message_iter_get_arg_type (&iter);
	}
      else if (strcmp (method, "ListManagers") == 0)
	{
	  expected_sig = "b";

	  if (arg_type == DBUS_TYPE_BOOLEAN)
	    /* Optional.  */
	    {
	      dbus_message_iter_get_basic (&iter, &recurse);

	      dbus_message_iter_next (&iter);
	      arg_type = dbus_message_iter_get_arg_type (&iter);
	    }
	}
      else
	expected_sig = "";

      if (arg_type != DBUS_TYPE_INVALID)
	goto bad_signature;

      GPtrArray *list = NULL;
      char *array_signature = NULL;
      if (type == root && strcmp (method, "ListManagers") == 0)
	{
	  ret = woodchuck_list_managers (recurse, &list, &error);
	  array_signature = "(ssss)";
	}
      else if (type == root && strcmp (method, "LookupManagerByCookie") == 0)
	{
	  ret = woodchuck_lookup_manager_by_cookie
	    (cookie, recurse, &list, &error);
	  array_signature = "(sss)";
	}
      else if (type == manager && strcmp (method, "LookupManagerByCookie") == 0)
	{
	  ret = woodchuck_manager_lookup_manager_by_cookie
	    (path, cookie, recurse, &list, &error);
	  array_signature = "(sss)";
	}
      else if (type == manager && strcmp (method, "ListManagers") == 0)
	{
	  ret = woodchuck_manager_list_managers (path, recurse,
						 &list, &error);
	  array_signature = "(ssss)";
	}
      else if (type == manager && strcmp (method, "ListStreams") == 0)
	{
	  ret = woodchuck_manager_list_streams (path, &list, &error);
	  array_signature = "(sss)";
	}
      else if (type == manager && strcmp (method, "LookupStreamByCookie") == 0)
	{
	  ret = woodchuck_manager_lookup_stream_by_cookie
	    (path, cookie, &list, &error);
	  array_signature = "(ss)";
	}
      else if (type == stream && strcmp (method, "ListObjects") == 0)
	{
	  ret = woodchuck_stream_list_objects (path, &list, &error);
	  array_signature = "(sss)";
	}
      else if (type == stream && strcmp (method, "LookupObjectByCookie") == 0)
	{
	  ret = woodchuck_stream_lookup_object_by_cookie
	    (path, cookie, &list, &error);
	  array_signature = "(ss)";
	}
      assert (array_signature);

      if (list)
	{
	  DBusMessageIter outer_iter;
	  dbus_message_iter_init_append (reply, &outer_iter);

	  DBusMessageIter array_iter;
	  dbus_message_iter_open_container (&outer_iter,
					    DBUS_TYPE_ARRAY,
					    array_signature,
					    &array_iter);

	  int i;
	  for (i = 0; i < list->len; i ++)
	    {
	      GPtrArray *strct = g_ptr_array_index (list, i);

	      DBusMessageIter struct_iter;
	      dbus_message_iter_open_container (&array_iter,
						DBUS_TYPE_STRUCT, NULL,
						&struct_iter);

	      int i;
	      for (i = 0; i < strct->len; i ++)
		{
		  char *value = g_ptr_array_index (strct, i);
		  if (! value)
		    value = "";

		  if (ret == 0)
		    dbus_message_iter_append_basic
		      (&struct_iter, DBUS_TYPE_STRING, &value);
		  g_free (g_ptr_array_index (strct, i));
		}

	      g_ptr_array_free (strct, TRUE);

	      dbus_message_iter_close_container (&array_iter, &struct_iter);
	    }

	  g_ptr_array_free (list, TRUE);

	  dbus_message_iter_close_container (&outer_iter, &array_iter);
	}
      else
	assert (ret != 0);
    }
  else if ((type == manager && strcmp (method, "Unregister") == 0)
	   || (type == stream && strcmp (method, "Unregister") == 0))
    {
      /* In.  */
      bool predicate = false;

      expected_sig = "b";
      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if (strcmp (expected_sig, actual_sig) != 0
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_BOOLEAN, &predicate,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      if (type == manager)
	ret = woodchuck_manager_unregister (path, predicate, &error);
      else
	{
	  assert (type == stream);
	  ret = woodchuck_stream_unregister (path, predicate, &error);
	}
    }
  else if (type == root && strcmp (method, "DownloadDesirability") == 0)
    {
      /* In.  */
      uint32_t request_type;
      struct woodchuck_download_desirability_version *versions = NULL;
      int version_count = 0;
      /* Out.  */
      uint32_t desirability = 0;
      uint32_t version = 0;

      expected_sig = "ua(xttu)";
      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if (strcmp (expected_sig, actual_sig) != 0
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_UINT32, &request_type,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      DBusMessageIter outer_iter;
      dbus_message_iter_init (message, &outer_iter);
      /* Skip request_type.  */
      dbus_message_iter_next (&outer_iter);

      DBusMessageIter array_iter;
      dbus_message_iter_recurse (&outer_iter, &array_iter);
      while (dbus_message_iter_get_arg_type (&array_iter)
	     != DBUS_TYPE_INVALID)
	version_count ++;

      versions = alloca (sizeof (versions[0]) * version_count);

      dbus_message_iter_recurse (&outer_iter, &array_iter);
      int i = 0;
      while (dbus_message_iter_get_arg_type (&array_iter)
	     != DBUS_TYPE_INVALID)
	{
	  DBusMessageIter struct_iter;
	  dbus_message_iter_recurse (&array_iter, &struct_iter);

	  dbus_message_iter_get_basic (&struct_iter,
				       &versions[i].expected_size);
	  dbus_message_iter_next (&struct_iter);
	  dbus_message_iter_get_basic (&struct_iter,
				       &versions[i].expected_transfer_up);
	  dbus_message_iter_next (&struct_iter);
	  dbus_message_iter_get_basic (&struct_iter,
				       &versions[i].expected_transfer_down);
	  dbus_message_iter_next (&struct_iter);
	  dbus_message_iter_get_basic (&struct_iter,
				       &versions[i].utility);

	  dbus_message_iter_next (&array_iter);
	  i ++;
	}

      ret = woodchuck_download_desirability_version
	(request_type, versions, version_count,
	 &desirability, &version, &error);

      if (ret == 0)
	dbus_message_append_args (reply, DBUS_TYPE_UINT32, &desirability,
				  DBUS_TYPE_UINT32, &version,
				  DBUS_TYPE_INVALID);
    }
  else if (type == manager && strcmp (method, "FeedbackSubscribe") == 0)
    {
      /* In.  */
      bool descendents_too;
      /* Out.  */
      char *handle = NULL;

      expected_sig = "b";
      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if (strcmp (expected_sig, actual_sig) != 0
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_BOOLEAN, &descendents_too,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      ret = woodchuck_manager_feedback_subscribe
	(dbus_message_get_sender (message),
	 path, descendents_too, &handle, &error);

      if (ret == 0)
	dbus_message_append_args (reply, DBUS_TYPE_STRING, &handle,
				  DBUS_TYPE_INVALID);

      g_free (handle);
    }
  else if (type == manager && strcmp (method, "FeedbackUnsubscribe") == 0)
    {
      /* In.  */
      const char *handle = NULL;

      expected_sig = "s";
      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if (strcmp (expected_sig, actual_sig) != 0
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_STRING, &handle,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      ret = woodchuck_manager_feedback_unsubscribe
	(dbus_message_get_sender (message), path, handle, &error);
    }
  else if (type == manager && strcmp (method, "FeedbackAck") == 0)
    {
      /* In.  */
      const char *object_uuid = NULL;
      unsigned int object_instance = 0;

      expected_sig = "su";
      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if (strcmp (expected_sig, actual_sig) != 0
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_STRING, &object_uuid,
				      DBUS_TYPE_UINT32, &object_instance,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      ret = woodchuck_manager_feedback_ack
	(dbus_message_get_sender (message), path, object_uuid, object_instance,
	 &error);
    }
  else if (type == object && strcmp (method, "Unregister") == 0)
    {
      expected_sig = "";
      if (strcmp (expected_sig, actual_sig) != 0)
	goto bad_signature;

      ret = woodchuck_object_unregister (path, &error);
    }
  else if (type == object && strcmp (method, "Download") == 0)
    {
      /* In.  */
      uint32_t request_type = 0;

      expected_sig = "u";
      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if (strcmp (expected_sig, actual_sig) != 0
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_UINT32, &request_type,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      ret = woodchuck_object_download (path, request_type, &error);
    }
  else if ((type == object && strcmp (method, "DownloadStatus") == 0)
	   || (type == stream && strcmp (method, "UpdateStatus") == 0))
    {
      /* In.  */
      uint32_t status;
      uint32_t indicator;
      uint64_t transferred_up;
      uint64_t transferred_down;
      uint64_t download_time;
      uint32_t download_duration;

      /* DownloadStatus.  */
      uint64_t object_size;
      struct woodchuck_object_download_status_files *files = NULL;
      int files_count = 0;

      /* UpdateStatus.  */
      uint32_t new_objects;
      uint32_t updated_objects;
      uint32_t objects_inline;

      if (strcmp (method, "DownloadStatus") == 0)
	expected_sig = "uutttuta(sbu)";
      else
	expected_sig = "uutttuuuu";

      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if (strcmp (expected_sig, actual_sig) != 0
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_UINT32, &status,
				      DBUS_TYPE_UINT32, &indicator,
				      DBUS_TYPE_UINT64, &transferred_up,
				      DBUS_TYPE_UINT64, &transferred_down,
				      DBUS_TYPE_UINT64, &download_time,
				      DBUS_TYPE_UINT32, &download_duration,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      DBusMessageIter outer_iter;
      dbus_message_iter_init (message, &outer_iter);
      /* Skip first 6 arguments.  */
      int i;
      for (i = 0; i < 6; i ++)
	dbus_message_iter_next (&outer_iter);

      if (strcmp (method, "DownloadStatus") == 0)
	{
	  dbus_message_iter_get_basic (&outer_iter, &object_size);
	  dbus_message_iter_next (&outer_iter);

	  DBusMessageIter array_iter;
	  dbus_message_iter_recurse (&outer_iter, &array_iter);
	  while (dbus_message_iter_get_arg_type (&array_iter)
		 != DBUS_TYPE_INVALID)
	    {
	      files_count ++;
	      dbus_message_iter_next (&array_iter);
	    }

	  files = alloca (sizeof (files[0]) * files_count);

	  dbus_message_iter_recurse (&outer_iter, &array_iter);
	  i = 0;
	  while (dbus_message_iter_get_arg_type (&array_iter)
		 != DBUS_TYPE_INVALID)
	    {
	      DBusMessageIter struct_iter;
	      dbus_message_iter_recurse (&array_iter, &struct_iter);

	      dbus_message_iter_get_basic (&struct_iter,
					   &files[i].filename);
	      dbus_message_iter_next (&struct_iter);
	      dbus_message_iter_get_basic (&struct_iter,
					   &files[i].dedicated);
	      dbus_message_iter_next (&struct_iter);
	      dbus_message_iter_get_basic (&struct_iter,
					   &files[i].deletion_policy);

	      dbus_message_iter_next (&array_iter);
	      i ++;
	    }

	  ret = woodchuck_object_download_status
	    (path, status, indicator, transferred_up, transferred_down,
	     download_time, download_duration, object_size,
	     files, files_count, &error);
	}
      else
	{
	  dbus_message_iter_get_basic (&outer_iter, &new_objects);
	  dbus_message_iter_next (&outer_iter);

	  dbus_message_iter_get_basic (&outer_iter, &updated_objects);
	  dbus_message_iter_next (&outer_iter);

	  dbus_message_iter_get_basic (&outer_iter, &objects_inline);
	  dbus_message_iter_next (&outer_iter);

	  ret = woodchuck_stream_update_status
	    (path, status, indicator, transferred_up, transferred_down,
	     download_time, download_duration, new_objects,
	     updated_objects, objects_inline, &error);
	}
    }
  else if (type == object && strcmp (method, "Used") == 0)
    {
      /* In.  */
      uint64_t start = 0;
      uint64_t duration = 0;
      uint64_t use_mask = 0;

      expected_sig = "ttt";
      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if (strcmp (expected_sig, actual_sig) != 0
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_UINT64, &start,
				      DBUS_TYPE_UINT64, &duration,
				      DBUS_TYPE_UINT64, &use_mask,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      ret = woodchuck_object_use (path, start, duration, use_mask, &error);
    }
  else if (type == object && strcmp (method, "FilesDeleted") == 0)
    {
      /* In.  */
      uint32_t update = 0;
      uint64_t arg = 0;

      expected_sig = "ut";
      DBusError dbus_error;
      dbus_error_init (&dbus_error);
      if (strcmp (expected_sig, actual_sig) != 0
	  || ! dbus_message_get_args (message, &dbus_error, 
				      DBUS_TYPE_UINT32, &update,
				      DBUS_TYPE_UINT64, &arg,
				      DBUS_TYPE_INVALID))
	{
	  dbus_error_free (&dbus_error);
	  goto bad_signature;
	}

      ret = woodchuck_object_files_deleted (path, update, arg, &error);
    }
  else
    {
    bad_method:
      error = g_error_new (DBUS_GERROR, DBUS_GERROR_UNKNOWN_METHOD,
			   "%s does not understand message %s.%s",
			   dbus_message_get_path (message), 
			   interface_str, method);
      if (! error_name)
	error_name = DBUS_ERROR_UNKNOWN_METHOD;
    }

 out:
  while (array_of_structs_to_free)
    {
      GPtrArray *a = array_of_structs_to_free->data;
      array_of_structs_to_free
	= g_slist_delete_link (array_of_structs_to_free,
			       array_of_structs_to_free);

      int i;
      for (i = 0; i < a->len; i ++)
	{
	  GValueArray *strct = g_ptr_array_index (a, i);
	  g_value_array_free (strct);
	}

      g_ptr_array_free (a, TRUE);
    }

  if (0)
    {
    bad_signature:
      error = g_error_new (DBUS_GERROR, DBUS_GERROR_INVALID_SIGNATURE,
			   "%s: %s.%s: Expected %s got %s.%s%s",
			   dbus_message_get_path (message), 
			   interface_str, method, expected_sig, actual_sig,
			   error_message ? " " : "", error_message ?: "");
      error_name = DBUS_ERROR_INVALID_ARGS;
    }
  if (ret != 0 && ! error)
    error = g_error_new (DBUS_GERROR, DBUS_GERROR_FAILED,
			 "%s: %s.%s: %s.",
			 dbus_message_get_path (message), 
			 interface_str, method,
			 error_message ?: woodchuck_error_to_error (ret));

  if (ret != 0 && ! error_name)
    error_name = woodchuck_error_to_error_name (ret);

  if (error)
    {
      assert (ret != 0);

      dbus_message_unref (reply);

      if (! error_name)
	error_name = DBUS_ERROR_FAILED;

      reply = dbus_message_new_error (message, error_name,
				      error->message);

      debug (3, "Returning %s: %s", error_name, error->message);

      g_error_free (error);
      error = NULL;
    }

  g_free (error_message);

  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);

  return DBUS_HANDLER_RESULT_HANDLED;
}

void
murmeltier_dbus_server_init (void)
{
  DBusError derror;
  dbus_error_init (&derror);

  DBusConnection *session_bus = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (! session_bus)
    {
      debug (0, "Failed to open connection to session bus: %s", derror.message);
      dbus_error_free (&derror);
    }

  static DBusObjectPathVTable org_woodchuck_vtable;
  org_woodchuck_vtable.message_function = process_message;
  dbus_connection_register_fallback (session_bus,
				     "/org/woodchuck",
				     &org_woodchuck_vtable, NULL);

  GError *error = NULL;
  DBusGConnection *g_session_bus = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
  if (g_session_bus == NULL)
    {
      debug (0, "Unable to connect to session bus: %s", error->message);
      g_error_free (error);
      return;
    }

  DBusGProxy *dbus_proxy
    = dbus_g_proxy_new_for_name (g_session_bus,
				 DBUS_SERVICE_DBUS,
				 DBUS_PATH_DBUS,
				 DBUS_INTERFACE_DBUS);

  char *bus_name = "org.woodchuck";
  guint ret;
  error = NULL;
  if (! org_freedesktop_DBus_request_name (dbus_proxy, bus_name,
					   DBUS_NAME_FLAG_DO_NOT_QUEUE, &ret,
					   &error))
    {
      debug (0, DEBUG_BOLD ("Unable to register service: %s"), error->message);
      g_error_free (error);
    }
  switch (ret)
    {
    case DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER:
      debug (5, "Acquired %s", bus_name);
      break;
    case DBUS_REQUEST_NAME_REPLY_IN_QUEUE:
      debug (0, "Waiting for bus name %s to become free", bus_name);
      abort ();
    case DBUS_REQUEST_NAME_REPLY_EXISTS:
      debug (0, "Bus name %s already owned.", bus_name);
      abort ();
    case DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER:
      debug (0, "We already own bus name?");
      abort ();
    default:
      debug (0, "Unknown return code: %d", ret);
      abort ();
    }

  g_object_unref (dbus_proxy);
}
