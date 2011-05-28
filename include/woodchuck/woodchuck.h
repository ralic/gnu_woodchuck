/* woodchuck.h - Woodchuck interface.
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
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#ifndef WOODCHUCK_WOODCHUCK_H
#define WOODCHUCK_WOODCHUCK_H

#include <stdint.h>
#include <glib-object.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

/* These error messages were added in Feb. 2011.
   http://lists.freedesktop.org/archives/dbus/2011-February/014107.html
 */
#ifndef DBUS_ERROR_UNKNOWN_OBJECT
# define DBUS_ERROR_UNKNOWN_OBJECT \
  "org.freedesktop.DBus.Error.UnknownObject"
#endif
#ifndef DBUS_ERROR_UNKNOWN_INTERFACE
# define DBUS_ERROR_UNKNOWN_INTERFACE \
  "org.freedesktop.DBus.Error.UnknownInterface"
#endif

enum woodchuck_error
  {
    WOODCHUCK_SUCCESS = 0,
    WOODCHUCK_ERROR_NO_SUCH_OBJECT = DBUS_GERROR_UNKNOWN_METHOD,
    WOODCHUCK_ERROR_GENERIC = 100,
    WOODCHUCK_ERROR_OBJECT_EXISTS,
    WOODCHUCK_ERROR_NOT_IMPLEMENTED,
    WOODCHUCK_ERROR_INTERNAL_ERROR,
    WOODCHUCK_ERROR_INVALID_ARGS,
  };

static inline const char *
woodchuck_error_to_error_name (enum woodchuck_error error)
{
  switch (error)
    {
    default:
      return "org.woodchuck.UnknownError";
    case WOODCHUCK_ERROR_GENERIC:
      return "org.woodchuck.GenericError";
    case WOODCHUCK_ERROR_NO_SUCH_OBJECT:
      return DBUS_ERROR_UNKNOWN_OBJECT;
    case WOODCHUCK_ERROR_OBJECT_EXISTS:
      return "org.woodchuck.ObjectExists";
    case WOODCHUCK_ERROR_NOT_IMPLEMENTED:
      return "org.woodchuck.MethodNotImplemented";
    case WOODCHUCK_ERROR_INTERNAL_ERROR:
      return "org.woodchuck.InternalError";
    case WOODCHUCK_ERROR_INVALID_ARGS:
      return "org.woodchuck.InvalidArgs";
    }
}

static inline const char *
woodchuck_error_to_error (enum woodchuck_error error)
{
  switch (error)
    {
    default:
    case WOODCHUCK_ERROR_GENERIC:
      return "Generic Error";
    case WOODCHUCK_ERROR_NO_SUCH_OBJECT:
      return "No such object";
    case WOODCHUCK_ERROR_OBJECT_EXISTS:
      return "Object exists.";
    case WOODCHUCK_ERROR_NOT_IMPLEMENTED:
      return "Method not implemented";
    case WOODCHUCK_ERROR_INTERNAL_ERROR:
      return "Internal server error";
    case WOODCHUCK_ERROR_INVALID_ARGS:
      return "Invalid arguments.";
    }
}

enum woodchuck_download_status
  {
    WOODCHUCK_DOWNLOAD_SUCCESS = 0,
    /* An unspecified transient error occured.  */
    WOODCHUCK_DOWNLOAD_FAILURE_TRANSIENT = 0x100,
    /* A network error occured and the server could not be
       reached.  */
    WOODCHUCK_DOWNLOAD_TRANSIENT_NETWORK = 0x101,
    /* The transfer was interrupted.  */
    WOODCHUCK_DOWNLOAD_TRANSIENT_INTERRUPTED = 0x102,

    /* An unspecified hard failure occured.  */
    WOODCHUCK_DOWNLOAD_FAILURE = 0x200,
    /* The file disappeared.  */
    WOODCHUCK_DOWNLOAD_FAILURE_GONE = 0x201,
  };

enum woodchuck_indicator
  {
    /* Audio sound.  */
    WOODCHUCK_INDICATOR_AUDIO = 0x1,
    WOODCHUCK_INDICATOR_APPLICATION_VISUAL = 0x2,
    /* A small icon blinks on the desktop.  */
    WOODCHUCK_INDICATOR_DESKTOP_SMALL_VISUAL = 0x4,
    /* A system tray notification message.  */
    WOODCHUCK_INDICATOR_DESKTOP_LARGE_VISUAL = 0x8,
    /* An externally visible notification, e.g., an LED.  */
    WOODCHUCK_INDICATOR_EXTERNAL_VISUAL = 0x10,
    WOODCHUCK_INDICATOR_VIBRATE = 0x20,

    WOODCHUCK_INDICATOR_OBJECT_SPECIFIC = 0x40,
    WOODCHUCK_INDICATOR_STREAM_WIDE = 0x80,
    WOODCHUCK_INDICATOR_MANAGER_WIDE = 0x100,

    WOODCHUCK_INDICATOR_UNKNOWN = 0x80000000,
  };

/* The deletion policy for files.  */
enum woodchuck_deletion_policy
  {
    /* This file is not eligible for deletion.  */
    WOODCHUCK_DELETION_POLICY_PRECIOUS = 0,
    /* Woodchuck may delete this file without consulting the
       application.  */
    WOODCHUCK_DELETION_POLICY_DELETE_WITHOUT_CONSULTATION = 1,
    /* This file may be deleted, but must be done by the application.
       In this case, the "object_delete" upcall will be invoked.  */
    WOODCHUCK_DELETION_POLICY_DELETE_WITH_CONSULTATION = 2,
  };

enum woodchuck_deletion_response
  {
    /* Files deleted.  */
    WOODCHUCK_DELETE_DELETED = 0,
    /* Deletion refused.  */
    WOODCHUCK_DELETE_REFUSED = 1,
    /* Files were compressed or partially deleted.  */
    WOODCHUCK_DELETE_COMPRESSED = 2,
  };

#endif
