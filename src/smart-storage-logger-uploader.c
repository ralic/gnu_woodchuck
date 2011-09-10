/* smart-storage-logger-uploader.c - Uploader.
   Copyright (C) 2010, 2011 Neal H. Walfield <neal@walfield.org>

   Woodchuck is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 3, or (at
   your option) any later version.

   Woodchuck is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <error.h>
#include <unistd.h>
#include <stdio.h>
#include <sqlite3.h>
#include <errno.h>
#include <glib.h>

#include "debug.h"
#include "sqlq.h"
#include "util.h"
#include "sqlq.h"
#include "files.h"

#include "network-monitor.h"
#include "user-activity-monitor.h"

#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include <obstack.h>

/* Smart storage logger registers tables (using
   logger_uploader_table_register) that should be uploaded.  This is
   one instance of this data structure for each such table.  This is
   saved persistently in upload.db. */
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

static sqlite3 *
uploader_db ()
{
  /* First, open the database.  */
  char *filename = files_logfile ("upload.db");

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  char *errmsg = NULL;
  err = sqlite3_exec
    (db,
     /* DB is the filename of the database.  TBL is the name of the
	table.  THROUGH is the LAST ROWID that has ben successfully
	uploaded.  */
     "create table if not exists status (db, tbl, through);"
     /* AT is the time the try occured.  SUCCESS is whether the update
	was successful (1) or failed (0).  OUTPUT is wget's
	output.  */
     "create table if not exists updates (at, success, output);",
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
logger_uploader_table_register (const char *filename, const char *table_name,
				bool delete)
{
  debug (5, "(%s, %s)", filename, table_name);

  /* Avoid calling malloc and free while holding the lock.  */
  struct db *db = malloc (sizeof (*db));
  db->filename = strdup (filename);
  struct table *table = calloc (sizeof (*table), 1);
  table->table = strdup (table_name);
  table->delete = delete;

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

static const char *
uuid (void)
{
  static char *my_uuid = NULL;
  if (my_uuid)
    return my_uuid;

  /* First, open the database.  */

  char *filename = files_logfile ("ssl.db");

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  char *errmsg = NULL;
  err = sqlite3_exec (db,
		      "create table if not exists uuid (uuid PRIMARY KEY);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  logger_uploader_table_register (filename, "uuid", false);

  /* Find the computer's UUID.  */
  int got = 0;
  int callback (void *cookie, int argc, char **argv, char **names)
  {
    assert (argc == 1);
    assert (! got);
    my_uuid = strdup (argv[0]);
    got ++;

    return 0;
  }
  sqlite3_exec (db, "select uuid from uuid;", callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%s: %s", filename, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  if (my_uuid)
    /* Got it!  */
    {
      sqlite3_close (db);
      free (filename);
      return my_uuid;
    }

  /* We need to generate a UUID.  */

  union
  {
    char str[33];
    uint32_t num[8];
  } uuid;
  memset (uuid.str, '0', 32);
  uuid.str[32] = 0;
  FILE *input
    = popen ("( ps aux; date; date +%N; echo $RANDOM; echo $RANDOM; echo $$;"
	     " uptime ) | md5sum", "r");
  if (input)
    {
      fread (uuid.str, 1, sizeof (uuid.str) - 1, input);
      pclose (input);
    }
  else
    debug (3, "Running (...) | md5sum: %m");

  void inplode (char in[32], char out[16])
  {
    int c2i (char c)
    {
      if ('0' <= c && c <= '9')
	return c - '0';
      if ('a' <= c && c <= 'f')
	return c - 'a' + 10;
      if ('A' <= c && c <= 'F')
	return c - 'a' + 10;
      return 0;
    }

    int i;
    for (i = 0; i < 32; i += 2)
      out[i/2] = c2i (in[i]) | (c2i (in[i + 1]) << 4);
  }
  inplode (uuid.str, uuid.str);

  uuid.num[0] ^= time (NULL);
  uuid.num[1] ^= getpid ();

  struct timeval tv;
  gettimeofday (&tv, NULL);
  uuid.num[2] ^= tv.tv_usec;

  union
  {
    double db[3];
    int num[0];
  } loadavg;
  getloadavg (loadavg.db, sizeof (loadavg.db) / sizeof (loadavg.db[0]));
  int x = 0;
  int i;
  for (i = 0; i < sizeof (loadavg) / sizeof (loadavg.num[0]); i ++)
    x += loadavg.num[i];
  uuid.num[3] ^= x;

  void explode (char in[16], char out[32])
  {
    char i2c (int i)
    {
      if (i < 10)
	return '0' + i;
      else if (i < 16)
	return 'a' + i - 10;
      assert (! "Bad i");
    }

    for (i = 0; i < 16; i ++)
      {
	out[i * 2] = i2c (((unsigned char) in[i]) & 0xF);
	out[i * 2 + 1] = i2c ((((unsigned char) in[i]) >> 4) & 0xF);
      }
  }

  char result[33];
  result[32] = 0;
  explode (uuid.str, result);
  my_uuid = strdup (result);

  /* Save the UUID.  */
  sqlite3_exec_printf (db,
		       "insert into uuid values (%Q);",
		       NULL, NULL, &errmsg, my_uuid);
  if (errmsg)
    {
      debug (0, "%s", errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  sqlite3_close (db);
  free (filename);

  return my_uuid;
}

struct upload_state
{
  char *upload_temp_filename;
  char *upload_filename;
  sqlite3 *db;

  struct obstack gather;
  struct obstack flush;

  /* The pipe.  */
  FILE *wget;
  /* The corresponding channel.  */
  GIOChannel *wget_channel;
  /* The gathered output.  */
  struct obstack wget_output;
  char *wget_output_str;
};

/* Whether an upload is in progress.  */
static bool uploading;

/* The time of the last (successful) upload.  */
static uint64_t last_upload;
/* The last time we tried to upload.  */
static uint64_t last_upload_try;

static gboolean upload_schedule (gpointer user_data);

static void
upload_state_free (struct upload_state *s, bool success)
{
  if (! s->wget_output_str)
    {
      /* NUL terminate the output.  */
      obstack_1grow (&s->wget_output, 0);
      s->wget_output_str = obstack_finish (&s->wget_output);
    }

  if (! success && s->db)
    {
      char *errmsg = NULL;
      int err = sqlite3_exec_printf
	(s->db, 
	 "attach '%s' as uploader;"
	 "insert into uploader.updates values (strftime('%%s','now'), %d, %Q);",
	 NULL, NULL, &errmsg, s->upload_filename, success, s->wget_output_str);
      if (errmsg)
	{
	  debug (0, "%d: %s", err, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	}
    }

  free (s->upload_filename);

  if (s->db)
    sqlite3_close (s->db);

  obstack_free (&s->gather, NULL);
  obstack_free (&s->flush, NULL);
  obstack_free (&s->wget_output, NULL);

  if (s->wget_channel)
    g_io_channel_unref (s->wget_channel);

  if (s->upload_temp_filename)
    {
      unlink (s->upload_temp_filename);
      free (s->upload_temp_filename);
    }

  if (s->wget)
    pclose (s->wget);

  g_free (s);

  uploading = false;

  if (success)
    last_upload = now ();
  else
    last_upload_try = now ();

  upload_schedule (NULL);
}

static gboolean
wget_output_cb (GIOChannel *source, GIOCondition condition, gpointer user_data)
{
  struct upload_state *s = user_data;

  gboolean success = FALSE;

  int room = obstack_room (&s->wget_output);
  char *p = obstack_next_free (&s->wget_output);
  bool copy = false;

  if (room == 0)
    /* We cannot write directly to the obstack.  We need to copy the
       data.  */
    {
      room = 1024;
      p = alloca (room);
      copy = true;
    }

  ssize_t r;
  if (condition == G_IO_IN)
    {
      r = read (fileno (s->wget), p, room);
      if (r > 0)
	/* Read data.  */
	{
	  if (copy)
	    obstack_grow (&s->wget_output, p, r);
	  else
	    obstack_blank_fast (&s->wget_output, r);

	  /* Call again.  */
	  return TRUE;
	}
    }
  else if (condition == G_IO_HUP)
    r = 0;
  else
    {
      r = -1;
      errno = 0;
    }

  /* EOF or error.  Either way, we need to clean up.  */

  if (r == -1)
    /* Error.  */
    {
      debug (0, "Reading from wget process: %m");

      upload_state_free (s, FALSE);

      /* Don't call again.  We're done.  */
      return FALSE;
    }


  /* EOF.  */

  /* NUL terminate the output.  */
  obstack_1grow (&s->wget_output, 0);
  s->wget_output_str = obstack_finish (&s->wget_output);

  /* Note: wget_status is as waitpid's status.  Because we do an open
     waitpid, pclose might not actually return the child's status but
     -1.  Instead, we grok the output.  */
  int wget_status = pclose (s->wget);
  s->wget = NULL;
  debug (3, "wget returned %d (exit code: %d) (%s, %d)",
	 wget_status, WEXITSTATUS (wget_status),
	 s->wget_output_str, (int) strlen (s->wget_output_str));

  if (strstr (s->wget_output_str, "\nDanke\n"))
    /* We got the expected server response.  */
    debug (3, "got expected server response.");
  else
    /* Assume an error occured.  */
    goto out;

  char *wget_output_quoted = sqlite3_mprintf ("%Q", s->wget_output_str);
  obstack_printf (&s->flush,
		  "insert into uploader.updates"
		  " values (strftime('%%s','now'), %d, %s);"
		  "end transaction;",
		  1, wget_output_quoted);
  sqlite3_free (wget_output_quoted);
  /* NUL terminate the SQL string.  */
  obstack_1grow (&s->flush, 0);
  char *sql = obstack_finish (&s->flush);

  char *errmsg = NULL;
  int err = sqlite3_exec (s->db, sql, NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
      /* We don't fail as the actual upload was correct.  Later, we'll
	 send some redundant data, but that's okay.  */
    }

  /* Move T->THROUGH to T->STAKE.  */
  struct db *d;
  for (d = dbs; d; d = d->next)
    {
      struct table *t;
      for (t = d->tables; t; t = t->next)
	t->through = t->stake;
    }

  success = TRUE;
 out:
  upload_state_free (s, success);

  /* Don't call again.  We're done.  */
  return FALSE;
}

/* Start an upload.  */
static void
upload (void)
{
  if (uploading)
    {
      debug (3, "Upload in progress.  Ignoring upload request.");
      return;
    }

  struct upload_state *s = g_malloc0 (sizeof (*s));

  s->upload_temp_filename = files_logfile ("upload-temp.db");
  /* Delete any existing database.  */
  unlink (s->upload_temp_filename);

  int err = sqlite3_open (s->upload_temp_filename, &s->db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   s->upload_temp_filename, sqlite3_errmsg (s->db));

  sqlite3_busy_timeout (s->db, 60 * 60 * 1000);

  obstack_init (&s->gather);
  obstack_init (&s->flush);
  obstack_init (&s->wget_output);

  uint64_t start = now ();

  obstack_printf (&s->gather, "begin transaction;");

  s->upload_filename = files_logfile ("upload.db");
  obstack_printf (&s->flush, "attach '%s' as uploader; begin transaction;",
		  s->upload_filename);

  /* Get the UUID and thereby ensure that the UUID table is
     registered.  */
  const char *my_uuid = uuid ();

  struct db *d;
  for (d = dbs; d; d = d->next)
    {
      char *dbname = sanitize_strings (strrchr (d->filename, '/') + 1, NULL);

      char *errmsg = NULL;
      err = sqlite3_exec_printf (s->db,
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
	  err = sqlite3_exec_printf (s->db,
				     "select max (ROWID) from %s.%s;",
				     callback, NULL, &errmsg, dbname, t->table);
	  if (errmsg)
	    {
	      debug (0, "%d: %s", err, errmsg);
	      sqlite3_free (errmsg);
	      errmsg = NULL;
	    }

	  debug (3, "%s.%s: %"PRId64" records need synchronization",
		 d->filename, t->table, t->stake - t->through);

	  char *name = sanitize_strings (dbname, t->table);
	  obstack_printf (&s->gather,
			  "create table %s as"
			  " select ROWID, * from %s.%s"
			  "  where %"PRId64" < ROWID and ROWID <= %"PRId64";",
			  name, dbname, t->table, t->through, t->stake);

	  if (t->delete)
	    /* Delete the synchronized records.  Note that we don't
	       actually delete the very last record just in case we
	       didn't set AUTOINCREMENT on the primary index.  */
	    obstack_printf (&s->flush,
			    "delete from %s.%s where ROWID < %"PRId64";",
			    dbname, t->table, t->stake);

	  /* Update through to avoid synchronizing the same data
	     multiple times.  */
	  obstack_printf (&s->flush,
			  "update uploader.status set through = %"PRId64
			  " where db = '%s' and tbl = '%s';",
			  t->stake, d->filename, t->table);

	  free (name);
	}

      free (dbname);
    }

  obstack_printf (&s->gather, "end transaction;");
  uint64_t mid = now ();
  /* NUL terminate the SQL string.  */
  obstack_1grow (&s->gather, 0);
  char *sql = obstack_finish (&s->gather);
  debug (5, "Copying: `%s'", sql);
  char *errmsg = NULL;
  err = sqlite3_exec (s->db, sql, NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
      upload_state_free (s, FALSE);
      return;
    }

  uint64_t end = now ();
  debug (3, "Prepare took "TIME_FMT"; flush took: "TIME_FMT,
	 TIME_PRINTF (mid - start),
	 TIME_PRINTF (end - mid));

  char *cmd = NULL;
  asprintf (&cmd,
	    "wget --tries=1 --post-file='%s'"
	    " --server-response --progress=dot"
	    " -O /dev/stdout -o /dev/stdout"
	    " --ca-certificate="PKGDATADIR"/ssl-receiver.cert"
	    " https://hssl.cs.jhu.edu:9321/%s 2>&1",
	    s->upload_temp_filename, my_uuid);
  debug (3, "Executing %s", cmd);
  s->wget = popen (cmd, "r");
  free (cmd);

  int fd = fileno (s->wget);
  if (fd == -1)
    {
      debug (0, "Failed to get underlying file descriptor.");
      upload_state_free (s, FALSE);
      return;
    }

  s->wget_channel = g_io_channel_unix_new (fd);

  uploading = true;
  g_io_add_watch (s->wget_channel,
		  G_IO_IN|G_IO_PRI|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
		  wget_output_cb, s);
}

#if 1
/* Don't start uploading unless we've been connected this long.  */
# define MIN_CONNECT_TIME (5 * 60 * 1000)
/* Upload data when it is this old.  */
# define SYNC_AGE (24 * 60 * 60 * 1000)
/* The amount of time that the system must be idle.  */
# define MIN_INACTIVITY (2 * 60 * 1000)
#else
/* Short values for debugging.  */
# define MIN_CONNECT_TIME (5 * 1000)
# define SYNC_AGE (10 * 1000)
# define MIN_INACTIVITY (5 * 1000)
#endif
  /* If we fail to upload wait about 5% of SYNC_AGE before trying
     again.  */
#define UPLOAD_RETRY_INTERVAL (SYNC_AGE / 20)

/* The time at which we got an acceptable default route.  */
static uint64_t connected;
/* The time the user went inactive (0 if the user is active).  */
static int64_t inactive;

static guint upload_schedule_source_id;

/* Check if all predicates have been met for a synchronization.  */
static gboolean
upload_schedule (gpointer user_data)
{
  uint64_t n = now ();

  debug (4, "Acceptable connection for "TIME_FMT"; inactive for "TIME_FMT"; "
	 "last upload "TIME_FMT" ago; last upload try "TIME_FMT" ago",
	 TIME_PRINTF (n - connected),
	 TIME_PRINTF (inactive == 0 ? 0 : (n - inactive)),
	 TIME_PRINTF (n - last_upload),
	 TIME_PRINTF (n - last_upload_try));

  if (upload_schedule_source_id)
    {
      g_source_remove (upload_schedule_source_id);
      upload_schedule_source_id = 0;
    }

  if (uploading || connected == 0 || inactive == 0)
    /* We must not be uploading, be connected and the user must be
       idle.  */
    {
      debug (3, "Upload predicates not satisfied "
	     "(uploading: %s; connected: %"PRId64"; inactive: %"PRId64").",
	     uploading ? "true" : "false", connected, inactive);
      return FALSE;
    }

  if (connected && n - connected >= MIN_CONNECT_TIME
      && inactive && n - inactive >= MIN_INACTIVITY
      && n - last_upload >= SYNC_AGE
      && n - last_upload_try >= UPLOAD_RETRY_INTERVAL)
    /* We've been connected long enough, the user has been inactive
       long enough, the data is old enough and we have not tried too
       recently.  Time to do an upload!  */
    {
      debug (3, "Starting upload.");

      upload ();
      return FALSE;
    }

  int64_t connect_timeout
    = MAX (MIN_CONNECT_TIME - (int64_t) (n - connected), 0);
  int64_t inactivity_timeout
    = MAX (MIN_INACTIVITY - (int64_t) (n - inactive), 0);
  int64_t age_timeout = MAX (SYNC_AGE - (int64_t) (n - last_upload), 0);
  int64_t retry_timeout
    = MAX (UPLOAD_RETRY_INTERVAL - (int64_t) (n - last_upload_try), 0);

  int64_t timeout = MAX5 (connect_timeout, inactivity_timeout,
			  age_timeout, retry_timeout, 1000);

  debug (3, "Timeout: "TIME_FMT" (connection: "TIME_FMT";"
	 " inactivity: "TIME_FMT"; next upload: "TIME_FMT";"
	 " next try: "TIME_FMT")",
	 TIME_PRINTF (timeout),
	 TIME_PRINTF (connect_timeout), TIME_PRINTF (inactivity_timeout),
	 TIME_PRINTF (age_timeout), TIME_PRINTF (retry_timeout));

  upload_schedule_source_id
    = g_timeout_add_seconds ((999 + timeout) / 1000, upload_schedule, NULL);

  return FALSE;
}

static void
default_connection_changed (NCNetworkMonitor *nm,
			    NCNetworkConnection *old_default,
			    NCNetworkConnection *new_default,
			    gpointer user_data)
{
  connected = 0;
  if (new_default)
    {
      uint32_t m = nc_network_connection_mediums (new_default);
      if (m & (NC_CONNECTION_MEDIUM_ETHERNET | NC_CONNECTION_MEDIUM_WIFI))
	{
	  connected = now ();
	  upload_schedule (NULL);
	}
    }
}

static void
idle_active (WCUserActivityMonitor *m,
	     int activity_status,
	     int activity_status_previous,
	     int64_t time_in_previous_state, gpointer user_data)
{
  inactive = 0;
  if (activity_status == WC_USER_IDLE)
    {
      inactive = now ();
      upload_schedule (NULL);
    }
}

void
logger_uploader_init (void)
{
  /* First, open the upload database.  */
  sqlite3 *db = uploader_db ();

  /* Try to recover LAST_UPLOAD and LAST_UPLOAD_TRY.  If we fail, it
     is okay: the defaults are reasonable.  */
  int callback (void *cookie, int argc, char **argv, char **names)
  {
    debug (3, "last_upload: %s; last_upload_try: %s", argv[0], argv[1]);
    if (argv[0])
      last_upload = atoll (argv[0]) * 1000;

    if (argv[1])
      last_upload_try = atoll (argv[1]) * 1000;
    else
      last_upload_try = last_upload;

    return 0;
  }

  char *errmsg = NULL;
  sqlite3_exec (db,
		"select (select max (at) from updates where success != 0),"
		"  (select max (at) from updates where success = 0);",
		callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "Getting upload status: %s", errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  /* Listen for events relevant to our predicate.  */
  NCNetworkMonitor *nm = nc_network_monitor_new ();
  g_signal_connect (G_OBJECT (nm), "default-connection-changed",
		    G_CALLBACK (default_connection_changed), NULL);
  default_connection_changed
    (nm, NULL, nc_network_monitor_default_connection (nm), NULL);

  WCUserActivityMonitor *m = wc_user_activity_monitor_new ();
  if (wc_user_activity_monitor_status (m) != WC_USER_ACTIVE)
    inactive = wc_user_activity_monitor_status_time_abs (m);
  g_signal_connect (G_OBJECT (m), "user-idle-active",
		    G_CALLBACK (idle_active), NULL);

  upload_schedule (NULL);
}
