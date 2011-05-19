/* gwoodchuck.h - A woodchuck library that integrated with glib.
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

#ifndef WOODCHUCK_GWOODCHUCK_H
#define WOODCHUCK_GWOODCHUCK_H

#include <stdint.h>
#include <glib-object.h>
#include <woodchuck/woodchuck.h>

typedef struct _GWoodchuck GWoodchuck;
typedef struct _GWoodchuckClass GWoodchuckClass;

#define GWOODCHUCK_TYPE (gwoodchuck_get_type ())
#define GWOODCHUCK(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GWOODCHUCK_TYPE, GWoodchuck))
#define GWOODCHUCK_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), GWOODCHUCK_TYPE, GWoodchuckClass))
#define IS_GWOODCHUCK(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GWOODCHUCK_TYPE))
#define IS_GWOODCHUCK_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GWOODCHUCK_TYPE))
#define GWOODCHUCK_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GWOODCHUCK_TYPE, GWoodchuckClass))

struct _GWoodchuckClass
{
  GObjectClass parent;
};

extern GType gwoodchuck_get_type (void);

struct gwoodchuck_vtable
{
  union
  {
    struct
    {
      /* Update the stream identified by STREAM_IDENTIFIER.

	 On completion, gwoodchuck_stream_updated or
	 gwoodchuck_stream_update_failed should be called, as appropriate.
	 Also, any new objects should be registered using
	 gwoodchuck_object_register.  If any objects were delivered
	 inline, then gwoodchuck_object_downloaded should also be called
	 on them.

	 Return 0 if the download will proceed.  Otherwise, return the
	 number of seconds to wait before the download should be
	 retried.  */
      uint32_t (*stream_update) (const char *stream_identifier,
				 gpointer user_data);

      /* Download the object identifier by STREAM_IDENTIFIER and
	 OBJECT_IDENTIFIER.  TARGET_QUALITY is a value from 1 to 5.  5
	 means to download the best version, 1 means to download the
	 version with the lowest-acceptable quality.

	 On completion, gwoodchuck_object_downloaded or
	 gwoodchuck_object_download_failed should be called, as
	 appropriate.

	 Return 0 if the download will proceed.  Otherwise, return the
	 number of seconds to wait before the download should be
	 retried.  */
      uint32_t (*object_download) (const char *stream_identifier,
				   const char *object_identifier,
				   uint32_t target_quality,
				   gpointer user_data);

      /* Delete the file identified by STREAM_IDENTIFIER and
	 OBJECT_IDENTIFIER.

	 If the object has been delete return 0.

	 If the object will not be deleted, return the number of
	 seconds to wait before rerequesting that the object be
	 deleted.

	 If the object has been compressed, return -1 * the new size
	 of the object.

	 To delay the decision, return a positive value, indicating
	 that the woodchuck server should wait before rerequesting
	 that the object be deleted and then follow up with a manual
	 call to gwoodchuck_object_files_deleted.  */
      int64_t (*object_delete) (const char *stream_identifier,
				const char *object_identifier,
				const char *filenames[],
				gpointer user_data);
    };

    /* Reserve space for future expansion.  */
    gpointer padding[16];
  };
};

/* Instantiate a woodchuck client object.  HUMAN_READABLE_NAME is a
   string that will be displayed to the user and should unambiguously
   identify the application (e.g., 'Foo EMail Client', 'Bar Podcast
   Manager').  DBUS_SERVICE_NAME is application's dbus service name.
   This is used as a unique identifier for the application and the end
   point used for upcalls.  */
extern GWoodchuck *gwoodchuck_new (const char *human_readable_name,
				   const char *dbus_service_name,
				   struct gwoodchuck_vtable *vtable,
				   gpointer user_data,
				   GError **error);

#define GWOODCHUCK_STREAM_UPDATE_HOURLY (60 * 60)
#define GWOODCHUCK_STREAM_UPDATE_EVERY_FEW_HOURS (6 * 60 * 60)
#define GWOODCHUCK_STREAM_UPDATE_DAILY (24 * 60 * 60)
#define GWOODCHUCK_STREAM_UPDATE_WEEKLY (7 * 24 * 60 * 60)
#define GWOODCHUCK_STREAM_UPDATE_MONTHLY (30 * 24 * 60 * 60)

/* Register a new stream.

   IDENTIFIER is a free-form string, which is uninterpreted by the
   server and provided on upcalls.  It must uniquely identify the
   stream within the application.  It can be an application specific
   key, e.g., the URL of an RSS feed.

   HUMAN_READABLE_NAME is a string that will be shown to the user and
   should unambiguously identify the stream in the context of the
   application.  For the "Foo Email Client," if there is only one
   Inbox, "Inbox" is sufficient for identifing the inbox stream; "Foo
   EMail Client: Inbox" is unnecessarily long.

   FRESHNESS is approximately how often the stream should be updated,
   in seconds.  This value is interpretted as a hint.  Woodchuck
   interprets 0 as meaning there are no freshness requirements and it
   is completely free to choose when to update the stream.  A value of
   UINT32_MAX is interpretted as meaning that the stream is never
   updated.  */
extern gboolean gwoodchuck_stream_register (GWoodchuck *wc,
					    const char *identifier,
					    const char *human_readable_name,
					    uint32_t freshness,
					    GError **error);

/* Indicate that the stream has been updated.

   TRANSFERRED is the number of bytes transferred.

   DURATION is the time required to perform the update, in
   seconds.

   NEW_OBJECTS is the number of new objects discovered.  (These should
   be registered with gwoodchuck_object_register.)

   UPDATED_OBJECTS is the number of objects for which updates are now
   available.  (This should consist only of objects that are already
   known.)

   OBJECTS_INLINE is the number of objects that were transferred
   inline.  This should be at most NEW_OBJECTS + UPDATED_OBJECTS.  */
extern gboolean gwoodchuck_stream_updated
  (GWoodchuck *wc, const char *stream_identifier,
   uint64_t transferred, uint32_t duration,
   uint32_t new_objects, uint32_t updated_objects, uint32_t objects_inline,
   GError **error);

/* Indicate that the stream has been updated, full version.

   INDICATOR_MASK: Set of indicators used to indicate an update to the
   user.  Bit-wise mask of enum woodchuck_indicator (see
   <woodchuck/woodchuck.h>).

   TRANSFERRED_UP is the number of bytes uploaded.

   TRANSFERRED_DOWN is the number of bytes downloaded.

   START is the time that the updated was initiated, in seconds since
   the epoch.

   DURATION is the time required to perform the update, in
   seconds.

   NEW_OBJECTS is the number of new objects discovered.  (These should
   be registered with gwoodchuck_object_register.)

   UPDATED_OBJECTS is the number of objects for which updates are now
   available.  (This should consist only of objects that are already
   known.)

   OBJECTS_INLINE is the number of objects that were transferred
   inline.  This should be at most NEW_OBJECTS + UPDATED_OBJECTS.  */
extern gboolean gwoodchuck_stream_updated_full
  (GWoodchuck *wc, const char *stream_identifier, uint32_t indicator_mask,
   uint64_t transferred_up, uint64_t transferred_down,
   uint64_t start, uint32_t duration,
   uint32_t new_objects, uint32_t updated_objects, uint32_t objects_inline,
   GError **error);

extern gboolean gwoodchuck_stream_update_failed (GWoodchuck *wc,
						 const char *stream_identifier,
						 uint32_t reason,
						 uint32_t transferred,
						 GError **error);

/* Delete the stream identified by STREAM_IDENTIFIER including any
   objects it contains.  */
extern gboolean gwoodchuck_stream_delete (GWoodchuck *wc,
					  const char *stream_identifier,
					  GError **error);

/* Register an object.

   STREAM_IDENTIFIER is object's stream.  It must have already been
   registered.

   OBJECT_IDENTIFIER is an identifier used to identifier the object in
   the stream.  It must be unique within the stream.

   HUMAN_READABLE_NAME is a string that will be shown to the user and
   should identify the object in the context of the stream.  For a
   blog post, the date and title might be a good example.

   EXPECTED_SIZE is the expected size of the object, in bytes.

   DOWNLOAD_FREQUENCY is how often the object should be updated (i.e.,
   the desired freshness), in seconds.  If the object is immutable,
   this should be set to 0, meaning that the object will be downloaded
   at most once.  */
extern gboolean gwoodchuck_object_register (GWoodchuck *wc,
					    const char *stream_identifier,
					    const char *object_identifier,
					    const char *human_readable_name,
					    uint64_t expected_size,
					    uint32_t download_frequency,
					    GError **error);

/* Delete the specified object.  */
extern gboolean gwoodchuck_object_delete (GWoodchuck *wc,
					  const char *stream_identifier,
					  const char *object_identifier,
					  GError **error);

/* Mark the object as having been downloaded.  If the object is not
   know, it is registered (using OBJECT_IDENTIFIER as the human
   readable name and assuming an immutable, i.e., one-show download,
   object).  */
extern gboolean gwoodchuck_object_downloaded
  (GWoodchuck *wc, const char *stream_identifier, const char *object_identifier,
   uint32_t indicator_mask, uint64_t object_size,
   uint32_t download_duration, const char *filename,
   uint32_t deletion_policy, GError **error);

struct gwoodchuck_object_downloaded_file
{
  const char *filename;
  gboolean dedicated;
  enum woodchuck_deletion_policy deletion_policy;
};

/* Mark the object as having been downloaded, full version.  */
extern gboolean gwoodchuck_object_downloaded_full
  (GWoodchuck *wc, const char *stream_identifier, const char *object_identifier,
   uint32_t indicator_mask, uint64_t transferred_up, uint64_t transferred_down,
   uint64_t download_time, uint32_t download_duration, uint64_t object_size,
   struct gwoodchuck_object_downloaded_file *files, int files_count,
   GError **error);

extern gboolean gwoodchuck_object_download_failed
  (GWoodchuck *wc, const char *stream_identifier, const char *object_identifier,
   uint32_t reason, uint32_t transferred, GError **error);

/* Return the desirability of downloading the object identified by
   OBJECT_IDENTIFIER right now.  */
extern gboolean gwoodchuck_object_download_desirability
(GWoodchuck *wc, const char *stream_identifier, const char *object_identifier,
 int *desirability, GError **error);

/* Mark the object identified by OBJECT_IDENTIFIER in the stream
   identified by STREAM_IDENTIFIER as having been recently used.  */
extern gboolean gwoodchuck_object_used (GWoodchuck *wc,
					const char *stream_identifier,
					const char *object_identifier,
					GError **error);

/* Mark the object identified by OBJECT_IDENTIFIER in the stream
   identified by STREAM_IDENTIFIER as having been used.  START and END
   indicate the time the user started using the object and stopped
   using the object, in seconds since the epoch.  USE_MASK indicates
   the portions of the object that have been used. Bit 0 corresponds
   to the first 1/64 of the object, bit 1 to the second 1/64 of the
   object, etc.  */
extern gboolean gwoodchuck_object_used_full (GWoodchuck *wc,
					     const char *stream_identifier,
					     const char *object_identifier,
					     uint64_t start, uint64_t end,
					     uint64_t use_mask,
					     GError **error);

extern gboolean gwoodchuck_object_files_deleted
  (GWoodchuck *wc, const char *stream_identifier, const char *object_identifier,
   enum woodchuck_delete_response response, uint64_t arg,
   GError **error);
						   
						 

#endif
