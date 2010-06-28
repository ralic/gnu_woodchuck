/* smart-storage-logger.c - Smart storage logger.
   Copyright (C) 2009, 2010 Neal H. Walfield <neal@walfield.org>

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

#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <error.h>
#include <errno.h>
#include <ftw.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdarg.h>
#include <sqlite3.h>
#include <signal.h>

#include "debug.h"
#include "list.h"
#include "btree.h"
#include "util.h"
#include "sqlq.h"
#include "pidfile.h"

#include "files.h"
#include "uploader.h"

static int inotify_fd;

const char *
uuid (void)
{
  static char *my_uuid = NULL;
  if (my_uuid)
    return my_uuid;

  /* First, open the database.  */

  char *filename = log_file ("uuid.db");

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  /* Figure out if the table has already been created.  */
  int count = 0;
  int tbl_callback (void *cookie, int argc, char **argv, char **names)
  {
    count = atoi (argv[0]);
    return 0;
  }

  char *errmsg = NULL;
  err = sqlite3_exec (db,
		      "select count (*) from sqlite_master"
		      " where type='table' and name='uuid';",
		      tbl_callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%s: %s", filename, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
      if (err != SQLITE_ERROR)
	abort ();
    }

  if (count == 0)
    /* No, create it.  */
    {
      char *errmsg = NULL;
      err = sqlite3_exec (db,
			  "create table uuid (uuid PRIMARY KEY);",
			  NULL, NULL, &errmsg);
      if (errmsg)
	{
	  debug (0, "%d: %s", err, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	}
    }
  else if (count != 1)
    /* Inconsistent.  */
    {
      debug (0, "%s has %d tables with name `uuid'?!?", filename, count);
      abort ();
    }

  uploader_table_register (filename, "uuid", false);

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
      debug (0, "UUID is %s", my_uuid);

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
    debug (0, "Running (...) | md5sum: %m");

  debug (0, "uuid first phase: %s", uuid.str);

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

  debug (0, "uuid final: %s", my_uuid);

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

  debug (0, "Generated UUID %s", my_uuid);

  return my_uuid;
}

static void
uuid_ensure (const char *filename, sqlite3 *db)
{
  /* Figure out if the table has already been created.  */
  int count = 0;
  int callback (void *cookie, int argc, char **argv, char **names)
  {
    count = atoi (argv[0]);
    return 0;
  }

  char *errmsg = NULL;
  int err = sqlite3_exec (db,
			  "select count (*) from sqlite_master"
			  " where type='table' and name='uuid';",
			  callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%s: %s", filename, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
      if (err != SQLITE_ERROR)
	abort ();
    }

  if (count == 0)
    /* No, create it.  */
    {
      err = sqlite3_exec_printf
	(db,
	 "begin transaction;"
	 "create table uuid (uuid PRIMARY KEY);"
	 "insert into uuid values (%Q);"
	 "commit transaction;",
	 NULL, NULL, &errmsg, uuid ());
      if (errmsg)
	{
	  debug (0, "%d: %s", err, errmsg);
	  sqlite3_free (errmsg);
	  sqlite3_exec (db, "rollback transaction;", NULL, NULL, NULL);
	}
    }
  else if (count != 1)
    /* Inconsistent.  */
    {
      debug (0, "%s has %d tables with name `uuid'?!?", filename, count);
      abort ();
    }


  count = 0;
  int uuid_check_callback (void *cookie, int argc, char **argv, char **names)
  {
    if (strcmp (argv[0], uuid ()) != 0)
      debug (0, "WARNING: %s: UUID %s does not match %s!",
	     filename, argv[0], uuid ());
    return 0;
  }

  err = sqlite3_exec (db,
		      "select uuid from uuid;",
		      uuid_check_callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%s: %s", filename, errmsg);
      sqlite3_free (errmsg);
      if (err != SQLITE_ERROR)
	abort ();
    }
}

static sqlite3 *
access_db_init (void)
{
  char *filename = log_file ("access.db");

  sqlite3 *access_db;
  int err = sqlite3_open (filename, &access_db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (access_db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (access_db, 60 * 60 * 1000);

  char *errmsg = NULL;
  err = sqlite3_exec (access_db,
		      /* This table maps filenames to uids and back.
			 It also records who owns the file (if
			 any).  */
		      "create table files "
		      " (uid INTEGER PRIMARY KEY,"
		      "  filename STRING NOT NULL UNIQUE,"
		      "  application STRING,"
		      "  file_group INTEGER"
		      " );"

		      /* This table is a log of all accesses.  */
		      "create table log "
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  uid INTEGER,"
		      "  time INTEGER,"
		      "  size_plus_one INTEGER);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
    }

  uploader_table_register (filename, "files", false);
  uploader_table_register (filename, "log", true);

  uuid_ensure (filename, access_db);

  free (filename);

  return access_db;
}

/* Each time we notice a change, first look to see if a notice node
   exists.  If it does not, we create a notice node.  Periodically, we
   flush notice nodes.  This allows us to aggregate multiple notices
   over a short period of time, which is a very common occurance.  */
struct notice_node
{
  btree_node_t tree_node;
  int mask;
  uint64_t time;
  char filename[];
};

int
notice_node_compare (const char *a, const char *b)
{
  return strcmp (a, b);
}

BTREE_CLASS (notice, struct notice_node, char, filename[0], tree_node,
	     notice_node_compare, false)

static pthread_mutex_t notice_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t notice_cond = PTHREAD_COND_INITIALIZER;

static btree_notice_t notice_tree_a;
static btree_notice_t notice_tree_b;
static btree_notice_t *notice_tree = &notice_tree_a;

static void
notice_add (const char *filename, int mask)
{
  pthread_mutex_lock (&notice_lock);

  bool broadcast = false;

  struct notice_node *n = btree_notice_find (notice_tree, filename);
  if (! n)
    {
      int l = strlen (filename);
      n = calloc (sizeof (*n) + l + 1, 1);
      memcpy (n->filename, filename, l + 1);
      n->time = now ();

      struct notice_node *o = btree_notice_insert (notice_tree, n);
      assert (! o);

      broadcast = true;
    }

  n->mask |= mask;

  pthread_mutex_unlock (&notice_lock);

  if (broadcast)
    pthread_cond_broadcast (&notice_cond);
}

static void *
notice_add_helper (void *arg)
{
  sqlite3 *access_db = access_db_init ();

  for (;;)
    {
      pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, NULL);
      pthread_testcancel ();
      sleep (5);

      pthread_mutex_lock (&notice_lock);
      struct notice_node *notice = btree_notice_first (notice_tree);
      if (! notice)
	{
	  debug (2, "No notices to process.");
	  pthread_cond_wait (&notice_cond, &notice_lock);
	  pthread_mutex_unlock (&notice_lock);
	  continue;
	}

      btree_notice_t *my_notice_tree = notice_tree;
      if (notice_tree == &notice_tree_a)
	notice_tree = &notice_tree_b;
      else
	notice_tree = &notice_tree_a;
      assert (btree_notice_first (notice_tree) == 0);

      pthread_mutex_unlock (&notice_lock);

      pthread_setcancelstate (PTHREAD_CANCEL_DISABLE, NULL);

      int processed = 0;

      void flush (const char *command_string1, const char *command_string2)
      {
	const char *c[2] = { command_string1, command_string2 };

	int i;
	for (i = 0; i < sizeof (c) / sizeof (c[0]); i ++)
	  if (c[i] && c[i][0] != 0)
	    break;
	if (i == sizeof (c) / sizeof (c[0]))
	  /* All strings are empty.  Nothing to do.  */
	  return;

	/* Wrap the command in a transaction.  */
	char *errmsg = NULL;
	sqlite3_exec (access_db, "begin transaction", NULL, NULL, &errmsg);
	if (errmsg)
	  {
	    debug (0, "begin transaction: %s", errmsg);
	    sqlite3_free (errmsg);
	    errmsg = NULL;

	    return;
	  }

	/* Execute the commands.  */
	for (i = 0; i < sizeof (c) / sizeof (c[0]); i ++)
	  {
	    if (! c[i] || c[i][0] == 0)
	      continue;

	    sqlite3_exec (access_db, c[i], NULL, NULL, &errmsg);
	    if (errmsg)
	      {
		debug (0, "%s -> %s", c[i], errmsg);
		sqlite3_free (errmsg);
		errmsg = NULL;
	      }
	  }
	
	sqlite3_exec (access_db, "end transaction", NULL, NULL, &errmsg);
	if (errmsg)
	  {
	    debug (0, "end transaction: %s", errmsg);
	    sqlite3_free (errmsg);
	    errmsg = NULL;

	    return;
	  }
      }

      char buffer[16 * 4096];
      struct sqlq *sqlq = sqlq_new_static (access_db, buffer, sizeof (buffer));

      do
	{
	  processed ++;
	  struct notice_node *next = btree_notice_next (notice);

	  const char *filename = notice->filename;
	  assert (filename);

	  if ((notice->mask & IN_ISDIR))
	    /* Ignore all directories.  */
	    {
	      debug (2, "Skipping directory %s", filename);
	      goto out;
	    }

	  uint64_t uid = 0;

	  /* Find the file's UID or add it to the database if we've
	     never seen it before.  */
	  int uid_callback (void *cookie, int argc, char **argv, char **names)
	  {
	    assert (argc == 1);
	    uid = atol (argv[0]);

	    return 0;
	  }

	  char *errmsg = NULL;
	  sqlite3_exec_printf (access_db,
			       "select uid from files where filename = %Q;",
			       uid_callback, NULL, &errmsg, filename);
	  if (errmsg)
	    {
	      debug (0, "%s: %s", filename, errmsg);
	      sqlite3_free (errmsg);
	      errmsg = NULL;
	    }

	  if (! uid)
	    /* It seems the file hasn't been seen before.  Insert it
	       into the database.  */
	    {
	      /* Append the string to the sql q then flush.  We must
		 flush as we need the result last_insert_rowid.  */
	      sqlq_append_printf (sqlq, true,
				  "insert into files (filename) values (%Q);",
				  filename);

	      uid = sqlite3_last_insert_rowid (access_db);
	    }

	  /* Recall: a size of 0 means deleted; any other value
	     corresponds to the size of the file plus one.  */
	  struct stat statbuf;
	  statbuf.st_size = 0;
	  if (stat (filename, &statbuf) == 0)
	    statbuf.st_size ++;

	  debug (2, "%d: %s: %"PRId64, processed, filename, uid);
	  sqlq_append_printf
	    (sqlq, false,
	     "insert into log (uid, time, size_plus_one)"
	     " values (%"PRId64",%"PRId64",%"PRId64");",
	     uid, notice->time / 1000, (uint64_t) statbuf.st_size);

	out:
	  free (notice);
	  notice = next;
	}
      while (notice);

      sqlq_flush (sqlq);

      debug (2, "Processed %d notices", processed);

      btree_notice_tree_init (my_notice_tree);
    }
}

/* Maps a watch identifier to a filename.  */
struct watch_node
{
  int watch;
  btree_node_t tree_node;
  char filename[];
};

int
watch_node_compare (const int *a, const int *b)
{
  return *a - *b;
}

BTREE_CLASS (watch, struct watch_node, int, watch, tree_node,
	     watch_node_compare, false)

static btree_watch_t watch_btree;

/* When a new directory is created, we do not scan it in the main
   thread but instantiate this data structure and enqueue it, which
   the helper thread eventually processes.  */
struct directory
{
  char *filename;
  struct list_node node;
};

LIST_CLASS(directory, struct directory, node, true)

static pthread_mutex_t directory_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t directory_cond = PTHREAD_COND_INITIALIZER;
struct directory_list directory_list;

static char *
directory_dequeue (void)
{
  char *filename = NULL;

  assert (pthread_mutex_trylock (&directory_lock) == EBUSY);

  struct directory *dir = directory_list_dequeue (&directory_list);
  if (dir)
    {
      filename = dir->filename;
      free (dir);
    }
  
  return filename;
}

static void *directory_add_helper (void *arg);

/* Recursively add all files starting at the filename designated by
   FILENAME.  FILENAME must be dominated by BASE (the root of the
   monitored hierarchy).  FILENAME is assumed to be in malloc'd
   storage.  This function assumes ownership of that memory.  */
static void
directory_add (char *filename)
{
  debug (5, "Adding %s", filename);

  assert (strncmp (filename, base, base_len) == 0);

  struct directory *dir = calloc (sizeof (*dir), 1);
  dir->filename = filename;

  pthread_mutex_lock (&directory_lock);

  directory_list_enqueue (&directory_list, dir);

  pthread_mutex_unlock (&directory_lock);
  pthread_cond_signal (&directory_cond);
}

static int total_watches;

static void *
directory_add_helper (void *arg)
{
  for (;;)
    {
      pthread_mutex_lock (&directory_lock);
      char *filename;
      for (;;)
	{
	  filename = directory_dequeue ();
	  if (filename)
	    break;
	  else
	    {
	      if (inotify_fd == -1)
		{
		  fprintf (stderr,
			   "Cannot add %d watches.  This is likely due to a "
			   "system configuration limitation.  Try changing "
			   "/proc/sys/fs/inotify/max_user_watches to be at "
			   "least as large as the above.\n",
			   total_watches);
		  exit (1);
		}

	      pthread_cond_wait (&directory_cond, &directory_lock);
	    }
	}
      pthread_mutex_unlock (&directory_lock);

      debug (2, "Processing %s", filename);

      int callback (const char *filename, const struct stat *stat,
		    int flag, struct FTW *ftw)
      {
	/* FILENAME is an absolute path.  */
	debug (5, " %s (%d)", filename, flag);

	if (flag == FTW_SL || flag == FTW_SLN)
	  {
	    debug (5, "Ignoring symbolic link (%s)", filename);
	    return 0;
	  }

	if (flag != FTW_D && flag != FTW_DP)
	  /* Only add directories.  */
	  {
	    debug (5, "Ignoring non-directory (%s)", filename);
	    return 0;
	  }

	if (under_dot_dir (filename))
	  {
	    debug (5, "Skipping %s (%s:%d)", filename, dot_dir, dot_dir_len);
	    return 0;
	  }

	if (inotify_fd >= 0)
	  {
	    int watch = inotify_add_watch (inotify_fd, filename,
					   IN_CREATE | IN_DELETE
					   | IN_DELETE_SELF
					   | IN_OPEN
					   | IN_CLOSE_WRITE
					   | IN_CLOSE_NOWRITE);
	    if (watch < 0)
	      switch (errno)
		{
		case EACCES:
		case ENOENT:
		case EPERM:
		  /* Ignore.  */
		  return 0;
		default:
		  inotify_fd = -1;
		  error (0, errno, "inotify_add_watch (%s) -> %d (%d total)",
			 filename, watch, total_watches);
		}

	    int len = strlen (filename);

	    struct watch_node *wn;

	    int l;
	    if (len == base_len)
	      {
		assert (strcmp (filename, base) == 0);
		l = 0;
	      }
	    else
	      /* Don't include a leading slash in the filename.  */
	      l = len - (base_len + 1);

	    wn = calloc (sizeof (*wn) + l + 1, 1);
	    wn->watch = watch;

	    if (len == base_len)
	      wn->filename[0] = 0;
	    else
	      memcpy (wn->filename, filename + base_len + 1, l + 1);
	    btree_watch_insert (&watch_btree, wn);
	  }

	total_watches ++;

	/* Continue processing.  */
	return 0;
      }

      /* Iterate over all files below FILENAME.  Don't follow symbolic
	 links. (256 = the maximum number of file descriptors to have
	 open.)  Do a depth first search.  The advantage to this is
	 that we don't get a lot of open events because we watch a
	 directory and then watch its contents.  */
      error_t err = nftw (filename, callback, 256, FTW_PHYS | FTW_DEPTH);
      if (err < 0)
	error (0, errno, "ftw (%s) %d", filename, errno);

      debug (2, "Processed %s (%d watches)", filename, total_watches);

      free (filename);
    }

  return NULL;
}

static char *
inotify_mask_to_string (uint32_t mask)
{
  char *str;

  asprintf (&str, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
	    (mask & IN_ACCESS) ? "IN_ACCESS " : "",
	    (mask & IN_MODIFY) ? "IN_MODIFY " : "",
	    (mask & IN_ATTRIB) ? "IN_ATTRIB " : "",
	    (mask & IN_CLOSE_WRITE) ? "IN_CLOSE_WRITE " : "",
	    (mask & IN_CLOSE_NOWRITE) ? "IN_CLOSE_NOWRITE " : "",
	    (mask & IN_OPEN) ? "IN_OPEN " : "",
	    (mask & IN_MOVED_FROM) ? "IN_MOVED_FROM " : "",
	    (mask & IN_MOVED_TO) ? "IN_MOVED_TO " : "",
	    (mask & IN_CREATE) ? "IN_CREATE " : "",
	    (mask & IN_DELETE) ? "IN_DELETE " : "",
	    (mask & IN_DELETE_SELF) ? "IN_DELETE_SELF " : "",
	    (mask & IN_MOVE_SELF) ? "IN_MOVE_SELF " : "",
	    (mask & IN_UNMOUNT) ? "IN_UNMOUNT " : "",
	    (mask & IN_Q_OVERFLOW) ? "IN_Q_OVERFLOW " : "",
	    (mask & IN_IGNORED) ? "IN_IGNORED " : "");
  if (*str)
    /* Remove the trailing space.  */
    str[strlen (str) - 1] = 0;

  return str;
}

#include <dbus/dbus.h>
#include "dbus-print-message.h"

static void *
battery_monitor (void *arg)
{
  /* First, open the database.  */

  char *filename = log_file ("battery.db");

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  char *errmsg = NULL;
  err = sqlite3_exec (db,
		      /* A list of batteries.  */
		      "create table batteries"
		      " (id INTEGER PRIMARY KEY,"
		      "  device,"
		      "  voltage_design, voltage_unit,"
		      "  reporting_design, reporting_unit);"

		      /* ID is the ID of the battery in the BATTERIES
			 table.  */
		      "create table battery_log"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  id, year, yday, hour, min, sec,"
		      "  is_charging, is_discharging,"
		      "  voltage, reporting, last_full);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  uploader_table_register (filename, "batteries", false);
  uploader_table_register (filename, "battery_log", true);

  uuid_ensure (filename, db);
  free (filename);

  /* Second, get the batteries.  */

  DBusError error;
  dbus_error_init (&error);

  DBusConnection *connection = dbus_bus_get_private (DBUS_BUS_SYSTEM, &error);
  if (connection == NULL)
    {
      debug (0, "Failed to open connection to bus: %s", error.message);
      dbus_error_free (&error);
      return NULL;
    }

  DBusMessage *message = dbus_message_new_method_call
    (/* Service.  */ "org.freedesktop.Hal",
     /* Object.  */ "/org/freedesktop/Hal/Manager",
     /* Interface.  */ "org.freedesktop.Hal.Manager",
     /* Method.  */ "FindDeviceByCapability"); 

  const char *type = "battery";
  dbus_message_append_args (message, DBUS_TYPE_STRING, &type,
			    DBUS_TYPE_INVALID);

  DBusMessage *reply = dbus_connection_send_with_reply_and_block
    (connection, message, -1 /* Don't timeout.  */, &error);
  if (dbus_error_is_set (&error))
    {
      debug (0, "Error sending to Hal: %s", error.message);
      dbus_error_free (&error);
      return NULL;
    }

  char **devices;
  int count;
  if (! dbus_message_get_args (reply, &error, 
			       DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
			       &devices, &count,
			       DBUS_TYPE_INVALID))
    {
      debug (0, "Failed to list batteries: %s", error.message);
      dbus_error_free (&error);
      return NULL;
    }
  dbus_message_unref (reply);
  dbus_message_unref (message);

  /* Print the results */

  /* Avoid nested functions with more than 4 arguments to work around
     a bug in Maemo 5's gcc:
     <https://bugs.maemo.org/show_bug.cgi?id=7699>. */
  struct lookup
  {
    const char *device;
    const char *method;
    const char *property;
    int type;
    void *locp;
  };

  bool lookup (struct lookup *lookup)
  {
    const char *device = lookup->device;
    const char *method = lookup->method;
    const char *property = lookup->property;
    int type = lookup->type;
    void *locp = lookup->locp;

    /* dbus-send --system --print-reply --dest=org.freedesktop.Hal
       /org/freedesktop/Hal/devices/computer_power_supply_battery_BAT0
       org.freedesktop.Hal.Device.GetPropertyStringList
       string:'battery.voltage.current'  */

    debug (2, "%s->%s (%s)", device, method, property);

    DBusMessage *message = dbus_message_new_method_call
      (/* Service.  */ "org.freedesktop.Hal",
       /* Object.  */ device,
       /* Interface.  */ "org.freedesktop.Hal.Device",
       /* Method.  */ method);

    dbus_message_append_args (message, DBUS_TYPE_STRING, &property,
			      DBUS_TYPE_INVALID);

    DBusError error;
    dbus_error_init (&error);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block
      (connection, message, 60 * 1000, &error);
    if (dbus_error_is_set (&error))
      {
	debug (0, "Error sending to Hal: %s", error.message);
	dbus_error_free (&error);
	return false;
      }

    if (! dbus_message_get_args (reply, &error, 
				 type, locp, DBUS_TYPE_INVALID))
      {
	debug (0, "Failed to list batteries: %s", error.message);
	dbus_error_free (&error);
	return false;
      }

    return true;
  }

  char *lookups (const char *device, const char *property)
  {
    char *result;
    struct lookup l = { device, "GetPropertyString", property,
			DBUS_TYPE_STRING, &result };
    if (lookup (&l))
      return result;
    else
      return NULL;
  }
 
  int lookupi (const char *device, const char *property)
  {
    int result;
    struct lookup l = { device, "GetPropertyInteger", property,
			DBUS_TYPE_INT32, &result };
    if (lookup (&l))
      return result;
    else
      return -1;
  }
 
  bool lookupb (const char *device, const char *property)
  {
    int result;
    struct lookup l = { device, "GetPropertyBoolean", property,
			DBUS_TYPE_BOOLEAN, &result };
    if (lookup (&l))
      return result;
    else
      return -1;
  }
 
  struct
  {
    int id;
    int is_charging;
    int is_discharging;
    int voltage;
    int reporting;
    int last_full;
  } battery[count];

  debug (0, "Batteries (%d):", count);
  int i;
  for (i = 0; i < count; i ++)
    {
      const char *device = devices[i];
      debug (0, "  %s\n", device);

      /* Set up a signal watcher for the device.  */
      char *match = NULL;
      if (asprintf (&match,
		    "type='signal',"
		    "interface='org.freedesktop.Hal.Device',"
		    "member='PropertyModified',"
		    "path='%s'", device) < 0)
	debug (0, "out of memory");

      if (match)
	{
	  debug (2, "Adding match %s", match);
	  dbus_bus_add_match (connection, match, &error);
	  free (match);
	  if (dbus_error_is_set (&error))
	    {
	      debug (0, "Error adding match: %s", error.message);
	      dbus_error_free (&error);
	    }
	}

      battery[i].id = -1;
      int present_callback (void *cookie, int argc, char **argv, char **names)
      {
	battery[i].id = atoi (argv[0]);
	return 0;
      }

      sqlite3_exec_printf (db, "select id from batteries where device = %Q",
			   present_callback, NULL, &errmsg,
			   battery);
      if (errmsg)
	{
	  debug (0, "%s: %s", filename, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	  continue;
	}

      debug (0, "ID is %d", battery[i].id);

      if (battery[i].id != -1)
	/* Already in the DB.  */
	continue;

      /* Add it to the DB.  */

      int voltage_design = lookupi (device, "battery.voltage.design");
      char *voltage_unit = lookups (device, "battery.voltage.unit");
      int reporting_design = lookupi (device, "battery.reporting.design");
      char *reporting_unit = lookups (device, "battery.reporting.unit");

      debug (1, "Battery %i (%s) ID: %d; "
	     "voltage design: %d %s; reporting design: %d %s",
	     i, device, battery[i].id,
	     voltage_design, voltage_unit, reporting_design, reporting_unit);

      sqlite3_exec_printf (db,
			   "insert into batteries"
			   " (device,  voltage_design, voltage_unit,"
			   "  reporting_design, reporting_unit)"
			   "values (%Q, %d, %Q, %d, %Q);",
			   NULL, NULL, &errmsg,
			   battery, voltage_design, voltage_unit,
			   reporting_design, reporting_unit);
      if (errmsg)
	{
	  debug (0, "%s", errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	  continue;
	}

      battery[i].id = sqlite3_last_insert_rowid (db);
    }

  /* Set up a filter that looks for notifications that a battery's
     status has changed.  */
  bool reread = true;
  DBusHandlerResult battery_callback (DBusConnection *connection,
				      DBusMessage *message, void *user_data)
  {
    int i;
    for (i = 0; i < count; i ++)
      if (battery[i].id != -1 && dbus_message_has_path (message, devices[i]))
	break;
    if (i == count)
      {
	debug (5, "Ignoring message with path %s",
	       dbus_message_get_path (message));
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      }

    if (dbus_message_is_signal (message,
				"org.freedesktop.Hal.Device",
				"PropertyModified"))
      {
	debug (5, "Processing property modified.");
	reread = true;
	return DBUS_HANDLER_RESULT_HANDLED;
      }

    do_debug (5)
      {
	debug (0, "Ignoring irrelevant method");
	print_message (message, false);
      }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  if (! dbus_connection_add_filter (connection, battery_callback, NULL, NULL))
    debug (0, "Failed to add filter: out of memory");

  while (dbus_connection_read_write_dispatch (connection,
					      /* No timeout.  */ -1))
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

      if (! reread)
	continue;

      struct tm tm = now_tm ();

      int i;
      for (i = 0; i < count; i ++)
	{
	  int id = battery[i].id;
	  const char *device = devices[i];

	  if (id == -1)
	    /* Battery not present.  */
	    continue;

	  int is_charging
	    = lookupb (device, "battery.rechargeable.is_charging");
	  int is_discharging
	    = lookupb (device, "battery.rechargeable.is_discharging");
	  int voltage = lookupi (device, "battery.voltage.current");
	  int reporting = lookupi (device, "battery.reporting.current");
	  int last_full = lookupi (device, "battery.reporting.last_full");

	  debug (1, "Battery %i (%s) ID: %d; %scharging, %sdischarging, "
		 "voltage: %d; reporting: %d; last_full: %d",
		 i, device, id,
		 is_charging ? "" : "not ", is_discharging ? "" : "not ",
		 voltage, reporting, last_full);

	  sqlite3_exec_printf (db,
			       "insert into battery_log"
			       " (id, year, yday, hour, min, sec,"
			       "  is_charging, is_discharging, voltage,"
			       "  reporting, last_full)"
			       "values (%d, %d, %d, %d, %d, %d,"
			       "        %d, %d, %d, %d, %d);",
			       NULL, NULL, &errmsg,
			       battery[i].id, tm.tm_year, tm.tm_yday,
			       tm.tm_hour, tm.tm_min, tm.tm_sec,
			       is_charging, is_discharging, voltage,
			       reporting, last_full);
	  if (errmsg)
	    {
	      debug (0, "%s", errmsg);
	      sqlite3_free (errmsg);
	      errmsg = NULL;
	      continue;
	    }

	}
    }

  dbus_free_string_array (devices);

  dbus_connection_close (connection);
  dbus_connection_unref (connection);

  return NULL;
}

#ifdef HAVE_MAEMO
/* ICD is maemo specific.  */

#include <icd/dbus_api.h>

static void *
network_monitor (void *arg)
{
  /* First, open the database.  */

  char *filename = log_file ("network.db");

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  char *errmsg = NULL;
  err = sqlite3_exec (db,
		      /* ID is the id of the connection.  It is
			 corresponds to the ROWID in CONNECTIONS.  */
		      "create table connection_log"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  year, yday, hour, min, sec,"
		      "  service_type, service_attributes, service_id,"
		      "  network_type, network_attributes, network_id, status);"

		      /* ID is the id of the connection.  It is
			 corresponds to the ROWID in CONNECTIONS.  */
		      "create table stats_log"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  year, yday, hour, min, sec,"
		      "  service_type, service_attributes, service_id,"
		      "  network_type, network_attributes, network_id,"
		      "  time_active, signal_strength, sent, received);"

		      /* Time that a scan was initiated.  ROWID
			 corresponds to ID in scan_log.  */
		      "create table scans"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  year, yday, hour, min, sec);"

		      /* ID corresponds to the ROWID of the scans table.  */
		      "create table scan_log"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT, id,"
		      "  status, last_seen,"
		      "  service_type, service_name, service_attributes,"
		      "	 service_id, service_priority,"
		      "	 network_type, network_name, network_attributes,"
		      "	 network_id, network_priority,"
		      "	 signal_strength, signal_strength_db,"
		      "  station_id);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  uploader_table_register (filename, "connection_log", true);
  uploader_table_register (filename, "stats_log", true);
  uploader_table_register (filename, "scans", true);
  uploader_table_register (filename, "scan_log", true);

  uuid_ensure (filename, db);
  free (filename);

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
	/* For network statistics.  */
	"type='signal',"
	"interface='"ICD_DBUS_API_INTERFACE"',"
	"member='"ICD_DBUS_API_STATISTICS_SIG"',"
	"path='"ICD_DBUS_API_PATH"'",
	/* For network scans.  */
	"type='signal',"
	"interface='"ICD_DBUS_API_INTERFACE"',"
	"member='"ICD_DBUS_API_SCAN_SIG"',"
	"path='"ICD_DBUS_API_PATH"'",

	/* System shutdown.  */
	"type='signal',"
	"interface='com.nokia.dsme.signal',"
	"path='/com/nokia/dsme/signal',"
	"member='shutdown_ind'",
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

  bool am_connected = true;
  uint64_t last_stats = 0;

  /* Number of network types being scanned.  */
  int am_scanning = 0;
  uint64_t last_scan_finished = 0;
  int scan_id = 0;

  uint64_t need_sql_flush = 0;
  uint64_t last_sql_append = 0;
  bool shutting_down = false;

  char buffer[16 * 4096];
  struct sqlq *sqlq = sqlq_new_static (db, buffer, sizeof (buffer));

  DBusHandlerResult network_callback (DBusConnection *connection,
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

    if (dbus_message_has_path (message, "/com/nokia/dsme/signal")
	&& dbus_message_is_signal (message,
				   "com.nokia.dsme.signal",
				   "shutdown_ind"))
      /* System shutdown.  */
      {
	debug (0, "System going down!");
	last_stats = 0;
	shutting_down = true;
      }
    else if (dbus_message_has_path (message, ICD_DBUS_API_PATH)
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

		struct tm tm = now_tm ();
		if (sqlq_append_printf
		    (sqlq, false,
		     "insert into connection_log "
		     " (year, yday, hour, min, sec,"
		     "  service_type, service_attributes, service_id,"
		     "  network_type, network_attributes, network_id, status)"
		     " values"
		     " (%d, %d, %d, %d, %d,"
		     "  %Q, %d, %Q, %Q, %d, %Q, %Q);",
		     tm.tm_year, tm.tm_yday, tm.tm_hour, tm.tm_min, tm.tm_sec,
		     service_type, service_attributes, service_id,
		     network_type, network_attributes, network_id,
		     status_to_str (status)))
		  {
		    if (! need_sql_flush)
		      need_sql_flush = now ();
		    last_sql_append = now ();
		  }
		else
		  need_sql_flush = last_sql_append = 0;

		if (status == ICD_STATE_CONNECTED)
		  am_connected = true;

		if (status == ICD_STATE_DISCONNECTING)
		  {
		    debug (1, "Getting stats for disconnecting connection.");

		    DBusMessage *message = dbus_message_new_method_call
		      (/* Service.  */ ICD_DBUS_API_INTERFACE,
		       /* Object.  */ ICD_DBUS_API_PATH,
		       /* Interface.  */ ICD_DBUS_API_INTERFACE,
		       /* Method.  */ ICD_DBUS_API_STATISTICS_REQ);

		    dbus_message_append_args
		      (message,
		       DBUS_TYPE_STRING, &service_type,
		       DBUS_TYPE_UINT32, &service_attributes,
		       DBUS_TYPE_STRING, &service_id,
		       DBUS_TYPE_STRING, &network_type,
		       DBUS_TYPE_UINT32, &network_attributes,
		       DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
		       &network_id, network_id_len,
		       DBUS_TYPE_INVALID);

		    DBusMessage *reply
		      = dbus_connection_send_with_reply_and_block
		      (connection, message, 60 * 1000, &error);
		    if (dbus_error_is_set (&error))
		      {
			debug (0, "Error sending to ICd2: %s", error.message);
			dbus_error_free (&error);
			goto stat_done;
		      }

		    int connected = 0;
		    if (! dbus_message_get_args (reply, &error, 
						 DBUS_TYPE_UINT32, &connected,
						 DBUS_TYPE_INVALID))
		      {
			debug (0, "Error parsing reply from ICd2: %s",
			       error.message);
			dbus_error_free (&error);
			goto stat_done;
		      }
		    else
		      debug (1, "connected: %d", connected);

		  stat_done:
		    dbus_message_unref (message);
		    if (reply)
		      dbus_message_unref (reply);
		  }
	      }
	  case 2:;

	    char *network_type = NULL;

	    /* Broadcast when a network search begins or ends.  */
	    if (! dbus_message_get_args (message, &error,
					 DBUS_TYPE_STRING, &network_type,
					 DBUS_TYPE_UINT32, &status,
					 DBUS_TYPE_INVALID))
	      {
		debug (0, "Failed to grok "ICD_DBUS_API_STATE_SIG" reply: %s",
		       error.message);
		dbus_error_free (&error);
	      }
	    else
	      {
		if (! am_scanning && status == ICD_STATE_SEARCH_START)
		  /* We are not scanning but we just saw a scan signal.  This
		     suggests the user might change connections soon.  Once a
		     connection is disconnected, we can't get statistics on it.
		     Preemptively try and get stats in case the user does
		     disconnect.  */
		  {
		    uint64_t n = now ();
		    int64_t delta = n - last_stats;
		    debug (0, "Not scanning but saw a scan (last stats: %"PRId64"s).", delta);
		    if (delta >= 1000)
		      {
			debug (0, "Preemptive stat scheduled.");
			last_stats = 0;
		      }
		  }
	      }

	  case 1:
	    /* Broadcast at startup if there are no connections.  */
	  default:
	    ;
	  }
      }
    else if (dbus_message_has_path (message, ICD_DBUS_API_PATH)
	     && dbus_message_is_signal (message,
					ICD_DBUS_API_INTERFACE,
					ICD_DBUS_API_STATISTICS_SIG))
      {
	char *service_type = NULL;
	uint32_t service_attributes = 0;
	char *service_id = NULL;
	char *network_type = NULL;
	uint32_t network_attributes = 0;
	char *network_id = NULL;
	int network_id_len = 0;
	uint32_t time_active = 0;
	int32_t signal_strength = 0;
	uint32_t sent = 0;
	uint32_t received = 0;

	DBusError error;
	dbus_error_init (&error);
	if (! dbus_message_get_args (message, &error,
				     DBUS_TYPE_STRING, &service_type,
				     DBUS_TYPE_UINT32, &service_attributes,
				     DBUS_TYPE_STRING, &service_id,
				     DBUS_TYPE_STRING, &network_type,
				     DBUS_TYPE_UINT32, &network_attributes,
				     DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
				     &network_id, &network_id_len,
				     DBUS_TYPE_UINT32, &time_active,
				     DBUS_TYPE_INT32, &signal_strength,
				     DBUS_TYPE_UINT32, &sent,
				     DBUS_TYPE_UINT32, &received,
				     DBUS_TYPE_INVALID))
	  {
	    debug (0, "Failed to grok "ICD_DBUS_API_STATISTICS_SIG" reply: %s",
		   error.message);
	    dbus_error_free (&error);
	  }
	else
	  {
	    debug (1, "Service: %s; %x; %s; "
		   "Network: %s; %x; %s; "
		   "active: %d; signal: %d; bytes: %d/%d",
		   service_type, service_attributes, service_id,
		   network_type, network_attributes, network_id,
		   time_active, signal_strength, sent, received);

	    struct tm tm = now_tm ();
	    if (sqlq_append_printf
		(sqlq, false,
		 "insert into stats_log"
		 "(year, yday, hour, min, sec,"
		 " service_type, service_attributes, service_id,"
		 " network_type, network_attributes, network_id,"
		 " time_active, signal_strength, sent, received)"
		 "values"
		 " (%d, %d, %d, %d, %d,"
		 "  %Q, %d, %Q, %Q, %d, %Q, %d, %d, %d, %d);",
		 tm.tm_year, tm.tm_yday, tm.tm_hour, tm.tm_min, tm.tm_sec,
		 service_type, service_attributes, service_id,
		 network_type, network_attributes, network_id,
		 time_active, signal_strength, sent, received))
	      {
		if (! need_sql_flush)
		  need_sql_flush = now ();
		last_sql_append = now ();
	      }
	    else
	      last_sql_append = now ();
	  }

	last_stats = now ();
      }
    else if (am_scanning
	     && dbus_message_has_path (message, ICD_DBUS_API_PATH)
	     && dbus_message_is_signal (message,
					ICD_DBUS_API_INTERFACE,
					ICD_DBUS_API_SCAN_SIG))
      {
	const char *scan_status_to_str (int scan_status)
	{
	  switch (scan_status)
	    {
	    case ICD_SCAN_NEW: return "new";
	    case ICD_SCAN_UPDATE: return "update";
	    case ICD_SCAN_NOTIFY: return "notify";
	    case ICD_SCAN_EXPIRE: return "expire";
	    case ICD_SCAN_COMPLETE: return "complete";
	    default: return "unknown status";
	    }
	}

	uint32_t status = 0;
	uint32_t last_seen = 0;
	char *service_type = NULL;
	char *service_name = NULL;
	uint32_t service_attributes = 0;
	char *service_id = NULL;
	int32_t service_priority = 0;
	char *network_type = NULL;
	char *network_name = NULL;
	uint32_t network_attributes = 0;
	char *network_id = NULL;
	int network_id_len = 0;
	int32_t network_priority = 0;
	int32_t signal_strength = 0;
	char *station_id = NULL;
	int32_t signal_strength_db = 0;

	DBusError error;
	dbus_error_init (&error);
	if (! dbus_message_get_args (message, &error,
				     DBUS_TYPE_UINT32, &status,
				     DBUS_TYPE_UINT32, &last_seen,
				     DBUS_TYPE_STRING, &service_type,
				     DBUS_TYPE_STRING, &service_name,
				     DBUS_TYPE_UINT32, &service_attributes,
				     DBUS_TYPE_STRING, &service_id,
				     DBUS_TYPE_INT32, &service_priority,
				     DBUS_TYPE_STRING, &network_type,
				     DBUS_TYPE_STRING, &network_name,
				     DBUS_TYPE_UINT32, &network_attributes,
				     DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
				     &network_id, &network_id_len,
				     DBUS_TYPE_INT32, &network_priority,
				     DBUS_TYPE_INT32, &signal_strength,
				     DBUS_TYPE_STRING, &station_id,
				     DBUS_TYPE_INT32, &signal_strength_db,
				     DBUS_TYPE_INVALID))
	  {
	    debug (0, "Failed to grok "ICD_DBUS_API_SCAN_SIG" reply: %s",
		   error.message);
	    dbus_error_free (&error);
	  }
	else
	  {
	    debug (1, DEBUG_BOLD ("Status: %s")"; last seen: %d; "
		   "Service: %s; %s; %x; %s; %d; "
		   "Network: %s; %s; %x; %s; %d; "
		   "signal: %d (%d dB); station: %s",
		   scan_status_to_str (status), last_seen,
		   service_type, service_name, service_attributes,
		   service_id, service_priority,
		   network_type, network_name, network_attributes,
		   network_id, network_priority,
		   signal_strength, signal_strength_db, station_id);

	    if (status != ICD_SCAN_COMPLETE)
	      {
		if (sqlq_append_printf
		    (sqlq, false,
		     "insert into scan_log "
		     "(id, status, last_seen,"
		     " service_type, service_name, service_attributes,"
		     " service_id, service_priority,"
		     " network_type, network_name, network_attributes,"
		     " network_id, network_priority,"
		     " signal_strength, signal_strength_db,"
		     " station_id)"
		     "values "		     
		     "(%d, %Q, %d, %Q, %Q, %d, %Q, %d,"
		     " %Q, %Q, %d, %Q, %d, %d, %d, %Q);",
		     scan_id,
		     scan_status_to_str (status), last_seen,
		     service_type, service_name,
		     service_attributes, service_id, service_priority,
		     network_type, network_name,
		     network_attributes, network_id, network_priority,
		     signal_strength, signal_strength_db, station_id))
		  {
		    if (! need_sql_flush)
		      need_sql_flush = now ();
		    last_sql_append = now ();
		  }
		else
		  need_sql_flush = last_sql_append = 0;
	      }

	    if (status == ICD_SCAN_COMPLETE)
	      {
		am_scanning --;
		if (am_scanning == 0)
		  {
		    debug (1, DEBUG_BOLD ("am_scanning 0, stopping scan"));

		    DBusMessage *message = dbus_message_new_method_call
		      (/* Service.  */ ICD_DBUS_API_INTERFACE,
		       /* Object.  */ ICD_DBUS_API_PATH,
		       /* Interface.  */ ICD_DBUS_API_INTERFACE,
		       /* Method.  */ ICD_DBUS_API_SCAN_CANCEL);

		    dbus_connection_send (connection, message, NULL);
		    dbus_message_unref (message);

		    last_scan_finished = now ();
		  }
	      }
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
  if (! dbus_connection_add_filter (connection, network_callback, NULL, NULL))
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


  void cleanup (void *arg)
  {
    sqlq_flush (sqlq);
  }
  pthread_cleanup_push (cleanup, NULL);

  /* When connected there is a network connection, get stats every
     few minutes or so.  */
#define STATS_FREQ (5 * 60 * 1000)
  /* Scan for available networks about every 3 hours.  */
#define SCAN_FREQ (3 * 60 * 60 * 1000)
  /* The maximum amount of time we are willing to tolerate data in the
     SQL buffer before it must be flushed and the maximum amount of
     IDLE time before we force a flush.  */
#define FLUSH_MAX_LATENCY (60 * 1000)
#define FLUSH_IDLE_LATENCY (2 * 1000)
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

      uint64_t n = now ();
      uint64_t delta;

      delta = n - last_stats;
      /* Read statistics.  */
      int64_t stat_timeout = INT64_MAX;
      if (am_connected)
	{
	  if (last_stats == 0 || delta >= STATS_FREQ - STATS_FREQ / 8)
	    /* Time (or, almost time) to get some new stats.  */
	    {
	      debug (1, "Requesting network statistics (last %d seconds ago).",
		     (int) (delta / 1000));

	      am_connected = false;

	      DBusMessage *message = dbus_message_new_method_call
		(/* Service.  */ ICD_DBUS_API_INTERFACE,
		 /* Object.  */ ICD_DBUS_API_PATH,
		 /* Interface.  */ ICD_DBUS_API_INTERFACE,
		 /* Method.  */ ICD_DBUS_API_STATISTICS_REQ);

	      DBusError error;
	      dbus_error_init (&error);

	      DBusMessage *reply = dbus_connection_send_with_reply_and_block
		(connection, message, 60 * 1000, &error);
	      if (dbus_error_is_set (&error))
		{
		  debug (0, "Error sending to ICd2: %s", error.message);
		  dbus_error_free (&error);
		  goto stat_done;
		}

	      int connected = 0;
	      if (! dbus_message_get_args (reply, &error, 
					   DBUS_TYPE_UINT32, &connected,
					   DBUS_TYPE_INVALID))
		{
		  debug (0, "Error parsing reply from ICd2: %s", error.message);
		  dbus_error_free (&error);
		  goto stat_done;
		}

	      debug (1, "%d statistics sent.", connected);

	      if (connected > 0)
		{
		  am_connected = true;
		  stat_timeout = STATS_FREQ;
		}

	      last_stats = n;

	    stat_done:
	      dbus_message_unref (message);
	      if (reply)
		dbus_message_unref (reply);
	    }
	  else
	    /* We need to wait before reading stats.  */
	    stat_timeout = last_stats + STATS_FREQ - n;
	}

      delta = n - last_scan_finished;
      int64_t scan_timeout = INT64_MAX;
      if (am_scanning <= 0)
	{
	  if (last_scan_finished == 0 || delta >= SCAN_FREQ)
	    /* Perform a scan if the last result was SCAN_FREQ in the
	       past.  */
	    {
	      debug (1, "Requesting network scan (last %d seconds ago).",
		     (int) (delta / 1000));

	      DBusMessage *message = dbus_message_new_method_call
		(/* Service.  */ ICD_DBUS_API_INTERFACE,
		 /* Object.  */ ICD_DBUS_API_PATH,
		 /* Interface.  */ ICD_DBUS_API_INTERFACE,
		 /* Method.  */ ICD_DBUS_API_SCAN_REQ);

	      uint32_t flags = ICD_SCAN_REQUEST_ACTIVE;
	      dbus_message_append_args (message,
					DBUS_TYPE_UINT32, &flags,
					DBUS_TYPE_INVALID);

	      DBusError error;
	      dbus_error_init (&error);

	      DBusMessage *reply = dbus_connection_send_with_reply_and_block
		(connection, message, 60 * 1000, &error);
	      if (dbus_error_is_set (&error))
		{
		  debug (0, "Error sending to ICd2: %s", error.message);
		  dbus_error_free (&error);
		  goto scan_done;
		}

	      char **networks;
	      dbus_error_init (&error);
	      if (! dbus_message_get_args (reply, &error,
					   DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
					   &networks, &am_scanning,
					   DBUS_TYPE_INVALID))
		{
		  debug (0, "Error parsing scan request reply: %s",
			 error.message);
		  dbus_error_free (&error);
		  goto scan_done;
		}
	      dbus_free_string_array (networks);

	      struct tm tm = now_tm ();

	      sqlq_append_printf (sqlq, true,
				  "insert into scans "
				  " (year, yday, hour, min, sec)"
				  " values (%d, %d, %d, %d, %d);",
				  tm.tm_year, tm.tm_yday,
				  tm.tm_hour, tm.tm_min, tm.tm_sec);
	      scan_id = sqlite3_last_insert_rowid (db);

	      debug (1, DEBUG_BOLD ("started scan %d.  (network types: %d)"),
		     scan_id, am_scanning);

	      if (am_scanning > 0)
		last_scan_finished = 0;
	      else
		{
		  scan_timeout = SCAN_FREQ;
		  last_scan_finished = n;
		}

	    scan_done:
	      dbus_message_unref (message);
	      if (reply)
		dbus_message_unref (reply);
	    }
	  else
	    /* We need to wait before reading stats.  */
	    scan_timeout = last_scan_finished + SCAN_FREQ - n;
	}


      int64_t flush_timeout = INT64_MAX;
      if (need_sql_flush)
	{
	  if (shutting_down
	      || n - need_sql_flush >= FLUSH_MAX_LATENCY
	      || n - last_sql_append >= FLUSH_IDLE_LATENCY)
	    {
	      debug (1, "Flushing SQL buffer.");
	      sqlq_flush (sqlq);
	      need_sql_flush = 0;
	    }
	  else
	    flush_timeout = last_sql_append + FLUSH_IDLE_LATENCY - n;
	}

      timeout = MIN (flush_timeout, MIN (stat_timeout, scan_timeout));

      debug (1, "Timeout: %"PRId64" s "
	     "(stat: %"PRId64"; scan: %"PRId64"; flush: %"PRId64")",
	     timeout == INT64_MAX ? -1 : (timeout / 1000),
	     stat_timeout == INT64_MAX ? -1 : (stat_timeout / 1000),
	     scan_timeout == INT64_MAX ? -1 : (scan_timeout / 1000),
	     flush_timeout == INT64_MAX ? -1 : (flush_timeout / 1000));
    }
  while (dbus_connection_read_write_dispatch
	 (connection, timeout == INT64_MAX ? -1: timeout));

  pthread_cleanup_pop (true);

  debug (0, "Network monitor disconnected.");

  return NULL;
}
#endif

static void *
process_monitor (void *arg)
{
  /* First, open the database.  */

  char *filename = log_file ("process.db");

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  char *errmsg = NULL;
  err = sqlite3_exec (db,
		      /* STATUS is either "acquired" or "released".  */
		      "create table process_log"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      "  year, yday, hour, min, sec, name, status);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  uploader_table_register (filename, "process_log", true);

  uuid_ensure (filename, db);
  free (filename);

  DBusError error;
  dbus_error_init (&error);

  DBusConnection *connection;
  int i;
  const int tries = 5;
  for (i = 0; i < tries; i ++)
    {
      connection = dbus_bus_get_private (DBUS_BUS_SESSION, &error);
      if (connection == NULL)
	{
	  debug (0, "Failed to open connection to bus: %s", error.message);
	  dbus_error_free (&error);

	  /* Maybe it the session dbus just hasn't started yet.  */
	  debug (0, "Waiting 60 seconds and trying again.");
	  sleep (60);
	}
    }
  if (i == tries)
    return NULL;

  /* Set up signal watchers.  */
  {
    char *matches[] =
      { /* For name changes. */
	"type='signal',"
	"interface='org.freedesktop.DBus',"
	"member='NameOwnerChanged',"
	"path='/org/freedesktop/DBus'",
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

  uint64_t need_sql_flush = 0;
  uint64_t last_sql_append = 0;

  char buffer[16 * 4096];
  struct sqlq *sqlq = sqlq_new_static (db, buffer, sizeof (buffer));


  DBusHandlerResult process_callback (DBusConnection *connection,
				      DBusMessage *message, void *user_data)
  {
    debug (2, "Got message (%p): %s->%s (%d args)",
	   message, dbus_message_get_path (message),
	   dbus_message_get_member (message),
	   dbus_message_args_count (message));
    // print_message (message, false);

    if (! dbus_message_has_path (message, "/org/freedesktop/DBus"))
      {
	debug (2, "Ignoring message with path %s",
	       dbus_message_get_path (message));
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      }

    const char *member = dbus_message_get_member (message);

    if (dbus_message_is_signal (message, "org.freedesktop.DBus",
				"NameOwnerChanged"))
      {
	char *name = NULL;
	char *old_owner = NULL;
	char *new_owner = NULL;

	dbus_error_init (&error);
	if (! dbus_message_get_args (message, &error,
				     DBUS_TYPE_STRING, &name,
				     DBUS_TYPE_STRING, &old_owner,
				     DBUS_TYPE_STRING, &new_owner,
				     DBUS_TYPE_INVALID))
	  {
	    debug (0, "Error parsing scan request reply: %s",
		   error.message);
	    dbus_error_free (&error);
	    goto change_done;
	  }

	debug (1, "name: %s; old_owner: %s; new_owner: %s",
	       name, old_owner, new_owner);

	if (name && name[0] != ':')
	  {
	    struct tm tm = now_tm ();
	    bool did_something = false;
	    bool need_flush = false;
	    if (old_owner && *old_owner)
	      {
		debug (0, "%s abandoned %s", old_owner, name);

		need_flush =
		  sqlq_append_printf
		  (sqlq, false,
		   "insert into process_log "
		   " (year, yday, hour, min, sec, name, status)"
		   " values (%d, %d, %d, %d, %d, %Q, 'released');",
		   tm.tm_year, tm.tm_yday, tm.tm_hour, tm.tm_min, tm.tm_sec,
		   name);
		did_something = true;
	      }
	    if (new_owner && *new_owner)
	      {
		debug (0, "%s assumed %s", new_owner, name);

		need_flush =
		  sqlq_append_printf
		  (sqlq, false,
		   "insert into process_log "
		   " (year, yday, hour, min, sec, name, status)"
		   " values (%d, %d, %d, %d, %d, %Q, 'acquired');",
		   tm.tm_year, tm.tm_yday, tm.tm_hour, tm.tm_min, tm.tm_sec,
		   name);
		did_something = true;
	      }

	    if (did_something)
	      {
		if (need_flush)
		  {
		    if (! need_sql_flush)
		      need_sql_flush = now ();
		    last_sql_append = now ();
		  }
		else
		  need_sql_flush = last_sql_append = 0;
	      }
	  }
      change_done:
	;
      }
    else
      {
	debug (5, "Ignoring irrelevant method: %s", member);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      }

    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (! dbus_connection_add_filter (connection, process_callback, NULL, NULL))
    debug (0, "Failed to add filter: out of memory");


  {
    struct tm tm = now_tm ();
    sqlq_append_printf (sqlq, false,
			"insert into process_log "
			" (year, yday, hour, min, sec, name, status)"
			" values (%d, %d, %d, %d, %d, '', 'system_start');",
			tm.tm_year, tm.tm_yday,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
  }

  void cleanup (void *arg)
  {
    sqlq_flush (sqlq);
  }
  pthread_cleanup_push (cleanup, NULL);

  /* The maximum amount of time we are willing to tolerate data in the
     SQL buffer before it must be flushed and the maximum amount of
     IDLE time before we force a flush.  */
#define FLUSH_MAX_LATENCY (60 * 1000)
#define FLUSH_IDLE_LATENCY (2 * 1000)
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

      uint64_t n = now ();

      int64_t flush_timeout = INT64_MAX;
      if (need_sql_flush)
	{
	  if (n - need_sql_flush >= FLUSH_MAX_LATENCY
	      || n - last_sql_append >= FLUSH_IDLE_LATENCY)
	    {
	      debug (0, "Flushing SQL buffer.");
	      sqlq_flush (sqlq);
	      need_sql_flush = 0;
	    }
	  else
	    {
	      flush_timeout = last_sql_append + FLUSH_IDLE_LATENCY - n;
	      debug (0, "Flushing SQL buffer in "TIME_FMT,
		     TIME_PRINTF (flush_timeout));
	    }
	}

      timeout = flush_timeout;

      debug (1, "Timeout: %"PRId64" s ",
	     timeout == INT64_MAX ? -1 : (timeout / 1000));
    }
  while (dbus_connection_read_write_dispatch
	 (connection, timeout == INT64_MAX ? -1: timeout));

  pthread_cleanup_pop (true);

  debug (0, "Process monitor disconnected.");

  return NULL;
}

static volatile int quit;

static pthread_t tid[6];

static void
signal_handler_quit (int sig)
{
  const char fmt[] = "Got signal %d.  Exiting.\n";
  char text[strlen (fmt) + 10];
  sprintf (text, fmt, sig);
  write (STDOUT_FILENO, text, strlen (text));

  quit = 1;

  int i;
  for (i = 0; i < sizeof (tid) / sizeof (tid[0]); i ++)
    pthread_cancel (tid[i]);
}

int
main (int argc, char *argv[])
{
  bool do_fork = true;

  {
    int i;
    for (i = 0; i < argc; i ++)
      if (strcmp (argv[i], "--no-fork") == 0)
	do_fork = false;
  }

  dbus_threads_init_default ();

  output_debug = 0;

  inotify_fd = inotify_init ();
  if (inotify_fd < 0)
    {
      fprintf (stderr, "Failed to initialize inotify.\n");
      return 1;
    }

  files_init ();

  char *pidfilename = log_file ("pid");
  const char *ssl = "smart-storage-logger";
  pid_t owner = pidfile_check (pidfilename, ssl);
  if (owner)
    error (1, 0, "%s already running (pid: %d)", ssl, owner);

  /* Make sure there are enough watches.  */
  {
    const char *WATCHES_FILE = "/proc/sys/fs/inotify/max_user_watches";
    FILE *watches_file = fopen (WATCHES_FILE, "r");
    int max = 0;
    fscanf (watches_file, "%d", &max);
    fclose (watches_file);

    int desired = 100000;
    if (max < desired)
      {
	watches_file = fopen (WATCHES_FILE, "w");
	if (! watches_file)
	  debug (0, "Cannot raise number of watches.  Stuck at %d.", max);
	else
	  {
	    fprintf (watches_file, "%d", desired);
	    fclose (watches_file);

	    debug (0, "Raised max watches from %d to %d", max, desired);
	  }
      }
  }

  
  char *log = log_file ("log");
  debug (0, "Daemonizing.  Further output will be sent to %s", log);

  int err;

  if (do_fork)
    {
      err = daemon (0, 0);
      if (err)
	error (0, err, "Failed to daemonize");
    }

  /* Redirect stdout and stderr to a log file.  */
  {
    int log_fd = open (log, O_WRONLY | O_CREAT | O_APPEND, 0660);
    dup2 (log_fd, STDOUT_FILENO);
    dup2 (log_fd, STDERR_FILENO);
    if (! (log_fd == STDOUT_FILENO || log_fd == STDERR_FILENO))
      close (log_fd);
  }
  free (log);
  debug (0, "Starting.");

  if ((owner = pidfile_acquire (pidfilename, ssl)))
    error (1, 0, "%s already running (pid: %d)", ssl, owner);
  free (pidfilename);

  /* Make sure the UUID is available before we create any threads: the
     function is not thread safe.  */
  uuid ();

  signal (SIGTERM, signal_handler_quit);
  signal (SIGINT, signal_handler_quit);
  signal (SIGQUIT, signal_handler_quit);
  signal (SIGHUP, signal_handler_quit);
  signal (SIGUSR1, signal_handler_quit);
  signal (SIGUSR2, signal_handler_quit);


  directory_add (strdup (base));


  /* Create the helper threads.  */

  /* Start watching a subtree.  */
  err = pthread_create (&tid[0], NULL, directory_add_helper, NULL);
  if (err < 0)
    error (1, errno, "pthread_create");

  /* Process enqueued inotify events.  */
  err = pthread_create (&tid[1], NULL, notice_add_helper, NULL);
  if (err < 0)
    error (1, errno, "pthread_create");

  /* Monitor batteries.  */
  err = pthread_create (&tid[2], NULL, battery_monitor, NULL);
  if (err < 0)
    error (1, errno, "pthread_create");

#ifdef HAVE_MAEMO
  /* Monitor network connections.  */
  err = pthread_create (&tid[3], NULL, network_monitor, NULL);
  if (err < 0)
    error (1, errno, "pthread_create");
#else
  tid[3] = 0;
#endif

  /* Monitor processes.  */
  err = pthread_create (&tid[4], NULL, process_monitor, NULL);
  if (err < 0)
    error (1, errno, "pthread_create");

  err = pthread_create (&tid[5], NULL, uploader_thread, NULL);
  if (err < 0)
    error (1, errno, "pthread_create (uploader_thread)");


  char buffer[16 * 4096];
  int have = 0;
  while (! quit)
    {
      if (inotify_fd == -1)
	/* While scanning, we ran out of space for watches.  This will
	   cause an error message to be displayed and the program to
	   exit--eventually.  */
	break;

      int len = read (inotify_fd, &buffer[have], sizeof (buffer) - have);
      debug (5, "read: %d", len);
      if (len < 0)
	{
	  if (errno == EINTR)
	    continue;
	  error (1, errno, "read");
	}

      len += have;
      have = 0;

      struct inotify_event *ev = (void *) buffer;
      while (len > 0)
	{
	  if (len < sizeof (*ev) + ev->len)
	    /* Don't have all the data from this event.  */
	    {
	      have = len;
	      memmove (buffer, ev, len);
	      break;
	    }

	  
	  char *events = NULL;
	  do_debug (2)
	    events = inotify_mask_to_string (ev->mask);

	  struct watch_node *wn = btree_watch_find (&watch_btree, &ev->wd);
	  if (! wn)
	    goto out;

	  char *element = ev->name;
	  if (ev->len == 0)
	    element = NULL;

	  char *filename = NULL;
	  asprintf (&filename, "%s%s%s%s%s",
		    base,
		    *wn->filename ? "/" : "", wn->filename,
		    element ? "/" : "", element ?: "");

	  if (! under_dot_dir (filename))
	    {
	      debug (2, "%s: %s (%x)", filename, events, ev->mask);
	      free (events);

	      if ((ev->mask & IN_CREATE))
		/* Directory was created.  */
		directory_add (strdup (filename));

	      if ((ev->mask & IN_DELETE_SELF))
		/* Directory was removed.  */
		{
		  debug (2, "Deleted: %s: %s (%x)",
			 filename, events, ev->mask);

		  inotify_rm_watch (inotify_fd, ev->wd);
		}

	      if ((ev->mask & IN_IGNORED))
		/* Watch was ignored either explicitly or implicitly.  */
		{
		  btree_watch_detach (&watch_btree, wn);
		  free (wn);
		  total_watches --;
		}

	      if (! (ev->mask & IN_ISDIR))
		/* Ignore directories.  */
		notice_add (filename, ev->mask);
	    }

	  free (filename);

	out:
	  len -= sizeof (*ev) + ev->len;
	  ev = (void *) ev + sizeof (*ev) + ev->len;
	}
    }

  int i;
  for (i = 0; i < sizeof (tid) / sizeof (tid[0]); i ++)
    if (tid[i] != 0)
      {
	debug (0, "Joining thread %d", i);
	pthread_join (tid[i], NULL);
      }

  debug (0, "Exiting.");

  return 0;
}
