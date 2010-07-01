/* uploader.c - Uploader.
   Copyright (C) 2010 Neal H. Walfield <neal@walfield.org>

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

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <error.h>
#include <unistd.h>
#include <sqlite3.h>
#include <dbus/dbus.h>
#include <icd/dbus_api.h>

#include "debug.h"
#include "sqlq.h"
#include "util.h"
#include "sqlq.h"
#include "files.h"

#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include <obstack.h>

struct table
{
  struct table *next;
  char *table;
  /* Whether synchronized entries should be deleted.  */
  bool delete;
  /* The ROWID of the last record saved for upload.  */
  uint64_t through;
  /* The ROWID of the last record queued for synchronization.  */
  uint64_t stake;
};

struct db
{
  struct db *next;
  char *filename;
  struct table *tables;
};
struct db *dbs;
static pthread_mutex_t dbs_lock = PTHREAD_MUTEX_INITIALIZER;

static sqlite3 *
uploader_db ()
{
  /* First, open the database.  */
  char *filename = log_file ("upload.db");

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  char *errmsg = NULL;
  err = sqlite3_exec (db,
		      "create table if not exists status (db, tbl, through);"
		      "create table if not exists updates (at, ret, output);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  free (filename);

  return db;
}

void
uploader_table_register (const char *filename, const char *table_name,
			 bool delete)
{
  debug (5, "(%s, %s)", filename, table_name);

  /* Avoid calling malloc and free while holding the lock.  */
  struct db *db = malloc (sizeof (*db));
  db->filename = strdup (filename);
  struct table *table = calloc (sizeof (*table), 1);
  table->table = strdup (table_name);
  table->delete = delete;

  pthread_mutex_lock (&dbs_lock);

  /* See if the DB has already been added.  */
  struct db *d;
  struct table *t;
  for (d = dbs; d; d = d->next)
    if (strcmp (d->filename, filename) == 0)
      /* A DB with this filename already exists.  */
      {
	/* See if the table has already been added.  */
	for (t = d->tables; t; t = t->next)
	  if (strcmp (t->table, table_name) == 0)
	    /* A table with this filename already exists.  */
	    {
	      debug (5, "Already added %s, %s!!!", filename, table_name);
	      assert (delete == t->delete);
	      goto out;
	    }

	/* We just need to add the table.  */
	debug (5, "DB %s exists, adding table %s", filename, table_name);

	t = table;
	table->next = d->tables;
	d->tables = table;

	/* Don't free TABLE.  */
	table = NULL;
	goto out;
      }

  /* We need to add the DB and the table.  */
  debug (5, "adding DB %s, adding table %s", filename, table_name);

  table->next = NULL;
  db->tables = table;
  db->next = dbs;
  dbs = db;
  t = table;

  /* Don't free DB or TABLE.  */
  db = NULL;
  table = NULL;

 out:
  pthread_mutex_unlock (&dbs_lock);

  if (table)
    {
      free (table->table);
      free (table);
    }
  if (db)
    {
      free (db->filename);
      free (db);
    }

  sqlite3 *uploader = uploader_db ();

  int callback (void *cookie, int argc, char **argv, char **names)
  {
    /* Already exists.  Save THROUGH and abort.  */
    t->through = argv[0] ? atoll (argv[0]) : 0;
    return 1;
  }

  char *errmsg = NULL;
  int err = sqlite3_exec_printf (uploader,
				 "select through from status"
				 " where db = '%s' and tbl = '%s';"
				 "insert into status values ('%s', '%s', 0);",
				 callback, NULL, &errmsg,
				 filename, table_name,
				 filename, table_name);
  if (errmsg)
    {
      if (err != SQLITE_ABORT)
	debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  sqlite3_close (uploader);
}

static char *
sanitize_strings (char *s1, char *s2)
{
  char *s = malloc (strlen (s1) + 1 + (s2 ? strlen (s2) : 0) + 1);
  strcpy (s, s1);
  s[strlen (s1)] = s2 ? '_' : 0;
  if (s2)
    strcpy (s + strlen (s1) + 1, s2);

  char *t;
  for (t = s; *t; t ++)
    if (('0' <= *t && *t <= '9')
	|| ('a' <= *t && *t <= 'z')
	|| ('A' <= *t && *t <= 'Z'))
      /* We're good.  */;
    else
      *t = '_';

  return s;
}

static bool
upload (void)
{
  bool ret = false;

  char *filename = log_file ("upload-temp.db");
  unlink (filename);

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  struct obstack gather;
  obstack_init (&gather);
  struct obstack flush;
  obstack_init (&flush);

  struct obstack wget_output_obstack;
  obstack_init (&wget_output_obstack);
  char *wget_output = NULL;
  int wget_status = -1;

  uint64_t start = now ();

  obstack_printf (&gather, "begin transaction;");

  char *upload_db = log_file ("upload.db");
  obstack_printf (&flush, "attach '%s' as uploader; begin transaction;",
		  upload_db);

  pthread_mutex_lock (&dbs_lock);

  struct db *d;
  for (d = dbs; d; d = d->next)
    {
      char *dbname = sanitize_strings (strrchr (d->filename, '/') + 1, NULL);

      char *errmsg = NULL;
      err = sqlite3_exec_printf (db,
				 "attach %Q as %s;",
				 NULL, NULL, &errmsg, d->filename, dbname);
      if (errmsg)
	{
	  debug (0, "%d: %s", err, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	}

      struct table *t;
      for (t = d->tables; t; t = t->next)
	{
	  int callback (void *cookie, int argc, char **argv, char **names)
	  {
	    /* If there are no records, argv[0] will be NULL.  */
	    t->stake = argv[0] ? atoll (argv[0]) : 0;
	    return 0;
	  }

	  char *errmsg = NULL;
	  err = sqlite3_exec_printf (db,
				     "select max (ROWID) from %s.%s;",
				     callback, NULL, &errmsg, dbname, t->table);
	  if (errmsg)
	    {
	      debug (0, "%d: %s", err, errmsg);
	      sqlite3_free (errmsg);
	      errmsg = NULL;
	    }

	  debug (0, "%s.%s: %"PRId64" records need synchronization",
		 d->filename, t->table, t->stake - t->through);

	  char *name = sanitize_strings (dbname, t->table);
	  obstack_printf (&gather,
			  "create table %s as"
			  " select ROWID, * from %s.%s"
			  "  where %"PRId64" < ROWID and ROWID <= %"PRId64";",
			  name, dbname, t->table, t->through, t->stake);

	  if (t->delete)
	    /* Delete the synchronized records.  Note that we don't
	       actually delete the very last record just in case we
	       didn't set AUTOINCREMENT on the primary index.  */
	    obstack_printf (&flush,
			    "delete from %s.%s where ROWID < %"PRId64";",
			    dbname, t->table, t->stake);

	  /* Update through to avoid synchronizing the same data
	     multiple times.  */
	  obstack_printf (&flush,
			  "update uploader.status set through = %"PRId64
			  " where db = '%s' and tbl = '%s';",
			  t->stake, d->filename, t->table);

	  free (name);
	}

      free (dbname);
    }

  pthread_mutex_unlock (&dbs_lock);

  obstack_printf (&gather, "end transaction;");
  uint64_t mid = now ();
  /* NUL terminate the SQL string.  */
  obstack_1grow (&gather, 0);
  char *sql = obstack_finish (&gather);
  debug (5, "Copying: `%s'", sql);
  char *errmsg = NULL;
  err = sqlite3_exec (db, sql, NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
      goto out;
    }

  uint64_t end = now ();
  debug (0, "Prepare took "TIME_FMT"; flush took: "TIME_FMT,
	 TIME_PRINTF (mid - start),
	 TIME_PRINTF (end - mid));

  extern const char *uuid (void);
  char *cmd = NULL;
  asprintf (&cmd,
	    "wget --tries=1 --post-file='%s' -O /dev/stdout -o /dev/stdout"
	    " -S --progress=dot"
	    " http://randal.lan:9321/%s 2>&1",
	    filename, uuid ());
  debug (0, "Executing %s", cmd);
  FILE *wget = popen (cmd, "r");
  free (cmd);

  ssize_t len;
  char *line = NULL;
  size_t size = 0;
  while ((len = getline (&line, &size, wget)) > 0)
    /* LEN includes a terminating newline (if any) and a NUL.  */
    {
      /* Remove the newline character.  */
      int l = len;
      /* Ignore the NUL.  */
      l --;
      obstack_grow (&wget_output_obstack, line, l);
      if (line[l] == '\n')
	line[l --] = 0;
      if (line[l] == '\r')
	line[l --] = 0;
      debug (0, "%s", line);
    }
  free (line);
  obstack_1grow (&wget_output_obstack, 0);
  wget_output = obstack_finish (&wget_output_obstack);

  wget_status = pclose (wget);
  debug (0, "wget returned %d (%s, %d)",
	 wget_status, wget_output, strlen (wget_output));

  if (wget_status != 0)
    /* An error occured.  Try again later.  */
    goto out;

  char *wget_output_quoted = sqlite3_vmprintf ("%Q", wget_output);
  obstack_printf (&flush,
		  "insert into uploader.updates"
		  " values (strftime('%%s','now'), %d, %s);"
		  "end transaction;",
		  wget_status, wget_output_quoted);
  sqlite3_free (wget_output_quoted);
  /* NUL terminate the SQL string.  */
  obstack_1grow (&flush, 0);
  sql = obstack_finish (&flush);

  err = sqlite3_exec (db, sql, NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
      /* We still return true as the actual upload went quite well.
	 Later, we'll send a bit more data, but that's okay.  */
    }

  /* Move T->THROUGH to T->STAKE.  */
  for (d = dbs; d; d = d->next)
    {
      struct table *t;
      for (t = d->tables; t; t = t->next)
	t->through = t->stake;
    }

  ret = true;
 out:
  if (! ret)
    {
      err = sqlite3_exec_printf (db, 
				 "attach '%s' as uploader;"
				 "insert into uploader.updates"
				 " values (strftime('%%s','now'), %d, %Q);",
				 NULL, NULL, &errmsg,
				 upload_db, wget_status, wget_output);
      if (errmsg)
	{
	  debug (0, "%d: %s", err, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	}
    }
  free (upload_db);

  sqlite3_close (db);
  obstack_free (&gather, NULL);
  obstack_free (&flush, NULL);
  obstack_free (&wget_output_obstack, NULL);
  unlink (filename);
  free (filename);
  return ret;
}

void *
uploader_thread (void *arg)
{
  /* First, open the database.  */
  sqlite3 *db = uploader_db ();

  /* We connect to the system bus to monitor when a network connection
     is available.  */
  DBusError error;
  dbus_error_init (&error);

  DBusConnection *connection = dbus_bus_get_private (DBUS_BUS_SYSTEM, &error);
  if (connection == NULL)
    {
      debug (0, "Failed to open connection to bus: %s", error.message);
      dbus_error_free (&error);
      return NULL;
    }

  /* Set up signal watchers.  */
  {
    char *matches[] =
      { /* For network state changes. */
	"type='signal',"
	"interface='"ICD_DBUS_API_INTERFACE"',"
	"member='"ICD_DBUS_API_STATE_SIG"',"
	"path='"ICD_DBUS_API_PATH"'",

	/* For activity/inactivity state changes. */
	"type='signal',"
	"interface='com.nokia.mce.signal',"
	"member='system_inactivity_ind',"
	"path='/com/nokia/mce/signal'",
      };

    int i;
    for (i = 0; i < sizeof (matches) / sizeof (matches[0]); i ++)
      {
	char *match = matches[i];

	debug (2, "Adding match %s", match);
	dbus_bus_add_match (connection, match, &error);
	if (dbus_error_is_set (&error))
	  {
	    debug (0, "Error adding match %s: %s", match, error.message);
	    dbus_error_free (&error);
	  }
      }
  }

  int64_t connected = 0;
  int64_t inactive = 0;

  char buffer[16 * 4096];
  struct sqlq *sqlq = sqlq_new_static (db, buffer, sizeof (buffer));

  DBusHandlerResult uploader_callback (DBusConnection *connection,
				       DBusMessage *message, void *user_data)
  {
    do_debug (5)
      {
	debug (0, "Got message (%p): %s->%s (%d args)",
	       message, dbus_message_get_path (message),
	       dbus_message_get_member (message),
	       dbus_message_args_count (message));
	// print_message (message, false);
      }

    if (dbus_message_has_path (message, ICD_DBUS_API_PATH)
	&& dbus_message_is_signal (message,
				   ICD_DBUS_API_INTERFACE,
				   ICD_DBUS_API_STATE_SIG))
      {
	const char *status_to_str (uint32_t status)
	{
	  switch (status)
	    {
	    case ICD_STATE_DISCONNECTED: return "disconnected";
	    case ICD_STATE_CONNECTING: return "connecting";
	    case ICD_STATE_CONNECTED: return "connected";
	    case ICD_STATE_DISCONNECTING: return "disconnecting";
	    case ICD_STATE_LIMITED_CONN_ENABLED: return "limited enabled";
	    case ICD_STATE_LIMITED_CONN_DISABLED: return "limited disabled";
	    case ICD_STATE_SEARCH_START: return "search start";
	    case ICD_STATE_SEARCH_STOP: return "search stop";
	    case ICD_STATE_INTERNAL_ADDRESS_ACQUIRED:
	      return "internal address acquired";
	    default:
	      debug (0, "Unknown network status code %d", status);
	      return "unknown";
	    }
	}
	char *service_type = NULL;
	uint32_t service_attributes = 0;
	char *service_id = NULL;
	char *network_type = NULL;
	uint32_t network_attributes = 0;
	char *network_id = NULL;
	int network_id_len = 0;
	char *conn_error = NULL;
	int32_t status = 0;

	DBusError error;
	dbus_error_init (&error);

	int args = dbus_message_args_count (message);
	switch (args)
	  {
	  case 8:
	    /* State of connection.  */
	    if (! dbus_message_get_args (message, &error,
					 DBUS_TYPE_STRING, &service_type,
					 DBUS_TYPE_UINT32, &service_attributes,
					 DBUS_TYPE_STRING, &service_id,
					 DBUS_TYPE_STRING, &network_type,
					 DBUS_TYPE_UINT32, &network_attributes,
					 DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
					 &network_id, &network_id_len,
					 DBUS_TYPE_STRING, &conn_error,
					 DBUS_TYPE_UINT32, &status,
					 DBUS_TYPE_INVALID))
	      {
		debug (0, "Failed to grok "ICD_DBUS_API_STATE_SIG" reply: %s",
		       error.message);
		dbus_error_free (&error);
	      }
	    else
	      {
		debug (1, "Service: %s; %x; %s; "
		       "Network: %s; %x; %s; status: %s",
		       service_type, service_attributes, service_id,
		       network_type, network_attributes, network_id,
		       status_to_str (status));

		if (status == ICD_STATE_CONNECTING)
		  {
		    debug (0, "Connecting to %s.", network_type);
		    if (strncmp (network_type, "WLAN_", 5) == 0)
		      {
			debug (0, "Setting connected timer (was: "TIME_FMT").",
			       TIME_PRINTF (connected == 0
					    ? -1 : now () - connected));
			connected = now ();
		      }
		  }
		if (status == ICD_STATE_CONNECTED)
		  {
		    debug (0, "Connected to %s.", network_type);
		    if (strncmp (network_type, "WLAN_", 5) == 0
			&& ! connected)
		      {
			debug (0, "Setting connected timer (was: "TIME_FMT").",
			       TIME_PRINTF (connected == 0
					    ? -1 : now () - connected));
			connected = now ();
		      }
		  }

		if (status == ICD_STATE_DISCONNECTED)
		  /* It is possible that there is another connection
		     that is active.  This happens when changing
		     connections: the old connection is only
		     disconnected after the new connection has been
		     connected.  To determine the current real state,
		     we reset the connected timer to 0 (= not
		     connected) and then send a state request.  If we
		     are connected, we'll get an ICD_STATE_CONNECTED
		     signal and if appropriate, this will set
		     CONNECTED.  */
		  {
		    debug (0, "Disconnected from %s", network_type);
		    debug (0, "Reseting connected timer (was: "TIME_FMT").",
			   TIME_PRINTF (connected == 0
					? -1 : now () - connected));
		    connected = 0;

		    DBusMessage *message = dbus_message_new_method_call
		      (/* Service.  */ ICD_DBUS_API_INTERFACE,
		       /* Object.  */ ICD_DBUS_API_PATH,
		       /* Interface.  */ ICD_DBUS_API_INTERFACE,
		       /* Method.  */ ICD_DBUS_API_STATE_REQ);

		    dbus_connection_send (connection, message, NULL);
		    dbus_message_unref (message);
		  }
	      }
	  case 2:
	    /* Broadcast when a network search begins or ends.  */
	  case 1:
	    /* Broadcast at startup if there are no connections.  */
	  default:
	    ;
	  }
      }
    else if (dbus_message_has_path (message, "/com/nokia/mce/signal")
	     && dbus_message_is_signal (message,
					"com.nokia.mce.signal",
					"system_inactivity_ind"))
      {
	bool inactive_p = false;
	if (! dbus_message_get_args (message, &error,
				     DBUS_TYPE_BOOLEAN, &inactive_p,
				     DBUS_TYPE_INVALID))
	  {
	    debug (0, "Failed to grok system_inactivity_ind: %s",
		   error.message);
	    dbus_error_free (&error);
	  }
	else
	  {
	    debug (1, "Inactive: %s", inactive_p ? "yes" : "no");
	    if (inactive_p)
	      inactive = now ();
	    else
	      inactive = 0;
	  }
      }
    else
      {
	debug (5, "Ignoring irrelevant method: %s",
	       dbus_message_get_member (message));
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      }

    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (! dbus_connection_add_filter (connection, uploader_callback, NULL, NULL))
    debug (0, "Failed to add filter: out of memory");


  /* Discover the state of any ongoing network connections.  */
  {
    DBusMessage *message = dbus_message_new_method_call
      (/* Service.  */ ICD_DBUS_API_INTERFACE,
       /* Object.  */ ICD_DBUS_API_PATH,
       /* Interface.  */ ICD_DBUS_API_INTERFACE,
       /* Method.  */ ICD_DBUS_API_STATE_REQ);

    dbus_connection_send (connection, message, NULL);
    dbus_message_unref (message);
  }


  /* Discover the device's activity state.  */
  {
    DBusMessage *message = dbus_message_new_method_call
      (/* Service.  */  "com.nokia.mce",
       /* Object.  */ "/com/nokia/mce/request",
       /* Interface.  */ "com.nokia.mce.request",
       /* Method.  */ "get_inactivity_status");

    DBusMessage *reply = dbus_connection_send_with_reply_and_block
      (connection, message, 60 * 1000, &error);
    dbus_message_unref (message);
    if (reply)
      {
	bool inactive_p = false;
	if (! dbus_message_get_args (reply, &error,
				     DBUS_TYPE_BOOLEAN, &inactive_p,
				     DBUS_TYPE_INVALID))
	  {
	    debug (0, "Failed to grok system_inactivity_ind: %s",
		   error.message);
	    dbus_error_free (&error);
	  }
	else
	  {
	    debug (1, "Inactive: %s", inactive_p ? "yes" : "no");
	    if (inactive_p)
	      inactive = now ();
	    else
	      inactive = 0;
	  }
	dbus_message_unref (reply);
      }
    else
      {
	debug (0, "Failed to grok system_inactivity_ind: %s",
	       error.message);
	dbus_error_free (&error);
      }
  }


  void cleanup (void *arg)
  {
    sqlq_flush (sqlq);
  }
  pthread_cleanup_push (cleanup, NULL);

#if 0
#define MIN_CONNECT_TIME (5 * 1000)
#define SYNC_AGE (10 * 1000)
#define INACTIVITY (5 * 1000)
#else
  /* Don't start uploading unless we've been connected this long.  */
#define MIN_CONNECT_TIME (5 * 60 * 1000)
  /* Upload data when it is this old.  */
#define SYNC_AGE (24 * 60 * 60 * 1000)
  /* The amount of time that the system must be idle.  */
#define INACTIVITY (2 * 60 * 1000)
#endif
  /* If we fail to upload wait about 5% of SYNC_AGE before trying
     again.  */
#define UPLOAD_RETRY_INTERVAL (SYNC_AGE / 20)

  int64_t last_upload = 0;
  int64_t last_upload_try = 0;
  {
    /* Try to recover LAST_UPLOAD and LAST_UPLAOD_TRY.  If we fail, it
       is okay: the defaults are reasonable.  */
    int callback (void *cookie, int argc, char **argv, char **names)
    {
      debug (0, "last_upload: %s; last_upload_try: %s", argv[0], argv[1]);
      if (argv[0])
	last_upload = atoll (argv[0]) * 1000;
      if (argv[1])
	last_upload_try = atoll (argv[1]) * 1000;
      return 0;
    }

    char *errmsg = NULL;
    sqlite3_exec (db,
		  "select (select max (at) from updates where ret = 0),"
		  "  (select max (at) from updates where ret != 0);",
		  callback, NULL, &errmsg);
    if (errmsg)
      {
	debug (0, "Getting upload status: %s", errmsg);
	sqlite3_free (errmsg);
	errmsg = NULL;
      }

    if (! last_upload)
      {
	if (last_upload_try)
	  /* We've tried to upload but never done so successfully.
	     Try soon.  */
	  last_upload = 0;
	else
	  /* We've never done an upload nor have we have tried.  Wait
	     the standard amount of time.  */
	  last_upload = now ();
      }
  }

  int64_t timeout;
  do
    {
      if (dbus_connection_get_dispatch_status (connection)
	  == DBUS_DISPATCH_DATA_REMAINS)
	/* There is more data to process, try to bulk up any
	   updates.  */
	{
	  while (dbus_connection_dispatch (connection)
		 == DBUS_DISPATCH_DATA_REMAINS)
	    ;
	}

      int64_t upload_timeout = INT64_MAX;
      if (connected)
	{
	  int64_t n = now ();

	  debug (1, "Connected "TIME_FMT"; inactive: "TIME_FMT"; "
		 "data's age "TIME_FMT"; last try: "TIME_FMT,
		 TIME_PRINTF (n - connected),
		 TIME_PRINTF (inactive == 0 ? 0 : (n - inactive)),
		 TIME_PRINTF (n - last_upload),
		 TIME_PRINTF (n - last_upload_try));

	  if (n - connected >= MIN_CONNECT_TIME
	      && inactive && n - inactive >= INACTIVITY
	      && n - last_upload >= SYNC_AGE
	      && n - last_upload_try >= UPLOAD_RETRY_INTERVAL)
	    /* Time to do an update!  */
	    {
	      debug (0, "Starting update.");

	      if (upload ())
		last_upload = n;
	      else
		/* Upload failed.  */
		last_upload_try = n;
	    }

	  int64_t connect_timeout = MIN_CONNECT_TIME - (n - connected);
	  int64_t inactivity_timeout
	    = inactive == 0 ? INT64_MAX : (INACTIVITY - (n - inactive));
	  int64_t age_timeout = SYNC_AGE - (n - last_upload);
	  int64_t retry_timeout
	    = UPLOAD_RETRY_INTERVAL - (n - last_upload_try);

	  upload_timeout = MAX4 (connect_timeout, inactivity_timeout,
				 age_timeout, retry_timeout);
	  debug (1, "Timeouts: connected "TIME_FMT"; "
		 "inactivity "TIME_FMT"; "
		 "data's age "TIME_FMT"; "
		 "last try: "TIME_FMT" => "TIME_FMT,
		 TIME_PRINTF (connect_timeout),
		 TIME_PRINTF (inactivity_timeout == INT64_MAX
			      ? -1 : inactivity_timeout),
		 TIME_PRINTF (age_timeout), TIME_PRINTF (retry_timeout),
		 TIME_PRINTF (upload_timeout));
	}

      timeout = upload_timeout;

      debug (1, "Timeout: "TIME_FMT,
	     TIME_PRINTF (timeout == INT64_MAX ? -1 : timeout));
    }
  while (dbus_connection_read_write_dispatch
	 (connection, timeout == INT64_MAX ? -1: MAX (2000, timeout)));

  pthread_cleanup_pop (true);

  debug (0, "Uploader disconnected.");

  return NULL;
}
