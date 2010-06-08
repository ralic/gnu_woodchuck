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

#include "debug.h"
#include "list.h"
#include "btree.h"
#include "util.h"

static int inotify_fd;

/* The directory under which we monitor files for changes (not
   including a trailing slash).  */
static char *base;
/* strlen (base).  */
static int base_len;

/* The directory (under the user's home directory) in which we store
   the log file.  */
#define DOT_DIR ".smart-storage"
/* The directory's absolute path.  */
static char *dot_dir;
/* strlen (dot_dir).  */
static int dot_dir_len;

/* Returns whether FILENAME is DOT_DIR or is under DOT_DIR.  */
static bool
under_dot_dir (const char *filename)
{
  return (strncmp (filename, dot_dir, dot_dir_len) == 0
	  && (filename[dot_dir_len] == '\0'
	      || filename[dot_dir_len] == '/'));
}

static sqlite3 *
access_db_init (void)
{
  /* This will fail in the common case that the directory already
     exists.  */
  mkdir (dot_dir, 0750);

  char *subdir = NULL;
  if (asprintf (&subdir, "%s/logs", dot_dir) < 0)
    error (1, 0, "out of memory");
  mkdir (subdir, 0750);
  free (subdir);

  char *filename = NULL;
  if (asprintf (&filename, "%s/logs/access.db", dot_dir) < 0)
    error (1, 0, "out of memory");

  sqlite3 *access_db;
  int err = sqlite3_open (filename, &access_db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (access_db));
  free (filename);

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (access_db, 60 * 60 * 1000);

  char *errmsg = NULL;
  err = sqlite3_exec (access_db,
		      "begin transaction;"

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
		      " (uid INTEGER,"
		      "  time INTEGER,"
		      "  size_plus_one INTEGER);"

		      "commit transaction;",
		      NULL, NULL, &errmsg);
  if (err)
    sqlite3_exec (access_db, "rollback transaction;", NULL, NULL, NULL);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
    }

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
      sleep (5);

      pthread_mutex_lock (&notice_lock);
      struct notice_node *notice = btree_notice_first (notice_tree);
      if (! notice)
	{
	  debug (1, "No notices to process.");
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

      char command_buffer[16 * 4096];
      int command_buffer_used = 0;

      /* Whether there is space for a string of length LEN (LEN does
	 not include the NUL terminator).  */
      bool have_space (int len)
      {
	return len + 1 <= sizeof (command_buffer) - command_buffer_used;
      }

      void append_hard (const char *s, int len)
      {
	assert (have_space (len));
	assert (! memchr (s, 0, len));

	memcpy (&command_buffer[command_buffer_used], s, len);
	command_buffer_used += len;
      }

      void append (char *command, bool force_flush)
      {
	int len = command ? strlen (command) : 0;
	if (force_flush || ! have_space (len))
	  /* Need to flush the buffer first.  */
	  {
	    /* NUL terminate the string.  */
	    command_buffer[command_buffer_used] = 0;
	    command_buffer_used = 0;
	    /* Flush the commands.  */
	    debug (1, "Flushing after %d records (`%s', `%s')",
		   processed, command_buffer, command);
	    flush (command_buffer, command);
	    return;
	  }

	if (! have_space (len))
	  /* The command is longer than we have space for.  Execute it
	     directly.  */
	  flush (command, NULL);
	else
	  append_hard (command, len);
      }

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
	      /* Append the string to the command buffer then flush.
		 We must flush as we need the result
		 last_insert_rowid.  */
	      char *sql = sqlite3_mprintf
		("insert into files (filename) values (%Q);", filename);
	      append (sql, true);
	      sqlite3_free (sql);

	      uid = sqlite3_last_insert_rowid (access_db);
	    }

	  /* Recall: a size of 0 means deleted; any other value
	     corresponds to the size of the file plus one.  */
	  struct stat statbuf;
	  statbuf.st_size = 0;
	  if (stat (filename, &statbuf) == 0)
	    statbuf.st_size ++;

	  debug (1, "%d: %s: %"PRId64, processed, filename, uid);
	  char *sql = sqlite3_mprintf
	    ("insert into log values (%"PRId64",%"PRId64",%"PRId64");",
	     uid, notice->time / 1000, (uint64_t) statbuf.st_size);
	  append (sql, false);
	  sqlite3_free (sql);

	out:
	  free (notice);
	  notice = next;
	}
      while (notice);

      append (NULL, true);
      assert (command_buffer_used == 0);

      debug (1, "Processed %d notices", processed);

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

      debug (1, "Processing %s", filename);

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

      debug (1, "Processed %s (%d watches)", filename, total_watches);

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

int
main (int argc, char *argv[])
{
  output_debug = 0;

  inotify_fd = inotify_init ();
  if (inotify_fd < 0)
    {
      fprintf (stderr, "Failed to initialize inotify.\n");
      return 1;
    }

  base = getenv ("HOME");
  base_len = strlen (base);

  asprintf (&dot_dir, "%s/"DOT_DIR, base);
  dot_dir_len = strlen (dot_dir);

  directory_add (strdup (base));


  /* Create the helper threads.  */
  pthread_t tid[2];
  int err = pthread_create (&tid[0], NULL, directory_add_helper, NULL);
  if (err < 0)
    error (1, errno, "pthread_create");

  err = pthread_create (&tid[1], NULL, notice_add_helper, NULL);
  if (err < 0)
    error (1, errno, "pthread_create");


  char buffer[16 * 4096];
  int have = 0;
  while (1)
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
	      debug (1, "%s: %s (%x)", filename, events, ev->mask);
	      free (events);

	      if ((ev->mask & IN_CREATE))
		/* Directory was created.  */
		directory_add (strdup (filename));

	      if ((ev->mask & IN_DELETE_SELF))
		/* Directory was removed.  */
		{
		  debug (1, "Deleted: %s: %s (%x)",
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

  pthread_join (tid[0], NULL);

  return 0;
}
