/* murmeltier-dbus-server.h - The dbus server details.
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

#ifndef MURMELTIER_DBUS_SERVER_H
#define MURMELTIER_DBUS_SERVER_H

#include "config.h"

#include "woodchuck/woodchuck.h"

#include <glib.h>

/* Initialize module.  */
extern void murmeltier_dbus_server_init (void);

/* org.woochuck callbacks.  */
extern enum woodchuck_error woodchuck_manager_register
  (GHashTable *properties, gboolean only_if_cookie_unique,
   char **uuid, GError **error);

/* Returns a GPtrArray of GPtrArray each containing four strings, the
   uuid, the cookie, the human readable name and the parent manager's
   UUID.  */
extern enum woodchuck_error woodchuck_list_managers
  (gboolean recurse, GPtrArray **list, GError **error);

/* Returns a GPtrArray of GPtrArray each containing three strings, the
   uuid, the human readable name and the parent manager's UUID.  */
extern enum woodchuck_error woodchuck_lookup_manager_by_cookie
  (const char *cookie, gboolean recursive, GPtrArray **list, GError **error);

struct woodchuck_download_desirability_version
{
  uint64_t expected_size;
  uint32_t utility;
};

extern enum woodchuck_error woodchuck_download_desirability_version
  (uint32_t request_type,
   struct woodchuck_download_desirability_version *versions, int version_count,
   uint32_t *desirability, uint32_t *version, GError **error);

/* org.woochuck.manager callbacks.  */
extern enum woodchuck_error woodchuck_manager_unregister
  (const char *manager, bool only_if_no_descendents, GError **error);

extern enum woodchuck_error woodchuck_manager_manager_register
  (const char *manager, GHashTable *properties, gboolean only_if_cookie_unique,
   char **uuid, GError **error);

/* Returns a GPtrArray of GPtrArray each containing four strings, the
   uuid, the cookie, the human readable name and the parent manager's
   UUID.  */
extern enum woodchuck_error woodchuck_manager_list_managers
  (const char *manager, gboolean recurse, GPtrArray **list, GError **error);

/* Returns a GPtrArray of GPtrArray each containing three strings, the
   uuid, the human readable name and the parent manager's UUID.  */
extern enum woodchuck_error woodchuck_manager_lookup_manager_by_cookie
(const char *manager, const char *cookie, gboolean recursive,
   GPtrArray **list, GError **error);

extern enum woodchuck_error woodchuck_manager_stream_register
  (const char *manager, GHashTable *properties, gboolean only_if_cookie_unique,
   char **uuid, GError **error);

/* Returns a GPtrArray of GPtrArray each containing three strings, the
   uuid, the cookie, and the human readable name.  */
extern enum woodchuck_error woodchuck_manager_list_streams
  (const char *manager, GPtrArray **list, GError **error);

/* Returns a GPtrArray of GPtrArray each containing two strings, the
   uuid and the human readable name.  */
extern enum woodchuck_error woodchuck_manager_lookup_stream_by_cookie
  (const char *manager, const char *cookie, GPtrArray **list, GError **error);

extern enum woodchuck_error woodchuck_manager_feedback_subscribe
  (const char *sender, const char *manager, bool descendents_too, char **handle,
   GError **error);

extern enum woodchuck_error woodchuck_manager_feedback_unsubscribe
  (const char *sender, const char *manager, const char *handle, GError **error);

extern enum woodchuck_error woodchuck_manager_feedback_ack
  (const char *sender, const char *manager,
   const char *object_uuid, uint32_t object_instance,
   GError **error);

/* org.woodchuck.stream callbacks.  */
extern enum woodchuck_error woodchuck_stream_unregister
  (const char *stream, bool only_if_empty, GError **error);

extern enum woodchuck_error woodchuck_stream_object_register
  (const char *stream, GHashTable *properties, gboolean only_if_cookie_unique,
   char **uuid, GError **error);

/* Returns a GPtrArray of GPtrArray each containing three strings, the
   uuid, the cookie and the human readable name.  */
extern enum woodchuck_error woodchuck_stream_list_objects
  (const char *stream, GPtrArray **list, GError **error);

/* Returns a GPtrArray of GPtrArray each containing two strings, the
   uuid and the human readable name.  */
extern enum woodchuck_error woodchuck_stream_lookup_object_by_cookie
  (const char *stream, const char *cookie, GPtrArray **list, GError **error);

extern enum woodchuck_error woodchuck_stream_update_status
  (const char *object, uint32_t status, uint32_t indicator,
   uint64_t transferred_up, uint64_t transferred_down,
   uint64_t download_time, uint32_t download_duration, 
   uint32_t new_objects, uint32_t updated_objects,
   uint32_t objects_inline, GError **error);

/* org.woodchuck.object callbacks.  */

extern enum woodchuck_error woodchuck_object_unregister
  (const char *object, GError **error);

extern enum woodchuck_error woodchuck_object_download
  (const char *object, uint32_t request_type, GError **error);

struct woodchuck_object_download_status_files
{
  const char *filename;
  gboolean dedicated;
  uint32_t deletion_policy;
};

extern enum woodchuck_error woodchuck_object_download_status
  (const char *object, uint32_t status, uint32_t indicator,
   uint64_t transferred_up, uint64_t transferred_down,
   uint64_t download_time, uint32_t download_duration, uint64_t object_size,
   struct woodchuck_object_download_status_files *files, int files_count,
   GError **error);

extern enum woodchuck_error woodchuck_object_use
  (const char *object, uint64_t start, uint64_t duration, uint64_t use_mask,
   GError **error);

extern enum woodchuck_error woodchuck_object_files_deleted
  (const char *object, uint32_t update, uint64_t arg, GError **error);

#endif
