/* smart-storage-logger.c - Smart storage logger.
   Copyright (C) 2009 Neal H. Walfield <neal@walfield.org>

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

static int inotify_fd;

static char *base;
static int base_len;

#define DOT_DIR ".smart-storage"
static char *dot_dir;
static int dot_dir_len;

static bool
under_dot_dir (const char *filename)
{
  return (strncmp (filename, dot_dir, dot_dir_len) == 0
	  && (filename[dot_dir_len] == '\0'
	      || filename[dot_dir_len] == '/'));
}

/* Return the time since the epoch with millisecond resolution.  */
static inline uint64_t
now (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return (uint64_t) tv.tv_sec * 1000ULL
    + (uint64_t) tv.tv_usec / 1000ULL;
}

#define TIME_FMT "%"PRId64" %s"
#define TIME_PRINTF(ms)				\
  ({						\
    int64_t ms_ = (ms);				\
    int neg = ms_ < 0;				\
    if (neg)					\
      ms_ = -ms_;				\
						\
    if (ms_ > 10 * 24 * 60 * 60 * 1000)		\
      ms_ /= 24 * 60 * 60 * 1000;		\
    else if (ms_ > 10 * 60 * 60 * 1000)		\
      ms_ /= 60 * 60 * 1000;			\
    else if (ms_ > 10 * 60 * 1000)		\
      ms_ /= 60 * 1000;				\
    else if (ms_ > 10 * 1000)			\
      ms_ /= 1000;				\
						\
    if (neg)					\
      ms_ = -ms_;				\
    ms_;					\
  }),						\
  ({						\
    int64_t ms_ = (ms);				\
    if (ms_ < 0)				\
  	ms_ = -ms_;				\
    char *s_ = "ms";				\
    if (ms_ > 10 * 24 * 60 * 60 * 1000)		\
      s_ = "days";				\
    else if (ms_ > 10 * 60 * 60 * 1000)		\
      s_ = "hours";				\
    else if (ms_ > 10 * 60 * 1000)		\
      s_ = "mins";				\
    else if (ms_ > 10 * 1000)			\
      s_ = "secs";				\
						\
    s_;						\
  })

/* A convenience function.  */
static int
sqlite3_exec_printf (sqlite3 *db, const char *sql,
		     int (*callback)(void*,int,char**,char**), void *cookie,
		     char **errmsg, ...)
{
  va_list ap;
  va_start (ap, errmsg);

  char *s = sqlite3_vmprintf (sql, ap);
  debug (5, "%s", s);

  int ret = sqlite3_exec (db, s, callback, cookie, errmsg);

  sqlite3_free (s);

  va_end (ap);

  return ret;
}

static sqlite3 *
access_db_init (void)
{
  /* This will fail in the common case that the directory already
     exists.  */
  mkdir (dot_dir, 0750);

  char *filename = NULL;
  asprintf (&filename, "%s/access.db", dot_dir);

  sqlite3 *access_db;
  int err = sqlite3_open (filename, &access_db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (access_db));

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

		      /* LAST_ACCESS is the last recorded access.  We
			 count at most one access per hour.  STARTX is
			 the time that the period was started.  COUNTX
			 is the number of accesses in that period.  X
			 is the number of days that that period
			 summarizes.  All periods overlap.  When an
			 access occurs and it occurs outside the
			 period, then the period must be
			 reinitialized.  */
		      "create table accesses "
		      " (uid INTEGER PRIMARY KEY,"
		      "  created INTEGER,"
		      "  last_access INTEGER,"
		      "  size_plus_one INTEGER,"
		      "  start1 INTEGER, count1 INTEGER,"
		      "  start1prev INTEGER, count1prev INTEGER,"
		      "  start2 INTEGER, count2 INTEGER,"
		      "  start2prev INTEGER, count2prev INTEGER,"
		      "  start4 INTEGER, count4 INTEGER,"
		      "  start4prev INTEGER, count4prev INTEGER,"
		      "  start8 INTEGER, count8 INTEGER,"
		      "  start8prev INTEGER, count8prev INTEGER,"
		      "  start16 INTEGER, count16 INTEGER,"
		      "  start16prev INTEGER, count16prev INTEGER,"
		      "  start32 INTEGER, count32 INTEGER,"
		      "  start32prev INTEGER, count32prev INTEGER,"
		      "  start64 INTEGER, count64 INTEGER,"
		      "  start64prev INTEGER, count64prev INTEGER,"
		      "  start128 INTEGER, count128 INTEGER,"
		      "  start128prev INTEGER, count128prev INTEGER,"
		      "  start256 INTEGER, count256 INTEGER,"
		      "  start256prev INTEGER, count256prev INTEGER"
		      " );"

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
	  debug (0, "No notices to process.");
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
      do
	{
	  processed ++;
	  struct notice_node *next = btree_notice_next (notice);

	  const char *filename = notice->filename;
	  assert (filename);

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
	    {
	      sqlite3_exec_printf
		(access_db,
		 "insert into files (filename) values (%Q);",
		 NULL, NULL, &errmsg, filename);
	      if (errmsg)
		error (1, 0, "%s:%d: %s", __FILE__, __LINE__, errmsg);
	      else
		uid = sqlite3_last_insert_rowid (access_db);
	    }

	  uint64_t n = now () / 1000;

	  /* Now, stamp the file.  */
	  /* Find the file's access record.  */
	  struct access
	  {
	    uint64_t start;
	    int count;
	    uint64_t startprev;
	    int countprev;
	  };
	  struct access a[9];
	  memset (a, 0, sizeof (a));
	  uint64_t last_access = n;
	  uint64_t created = n;

	  bool needs_update = true;

	  int access_callback (void *cookie, int argc, char **argv,
			       char **names)
	  {
	    // uid = atol (argv[0]);
	    created = atol (argv[1]);
	    last_access = atol (argv[2]);
	    // size = atol (argv[3]);

	    debug (0, "%s updated "TIME_FMT" ago (%"PRId64"-%"PRId64").",
		   filename, TIME_PRINTF ((n - last_access) * 1000),
		   n, last_access);

	    if (last_access + 60 * 60 > n)
	      /* The last access was less than an hour in the past.  Don't
		 update.  */
	      {
		needs_update = false;
		return 0;
	      }

	    int i;
	    for (i = 0; i < sizeof (a) / sizeof (a[0]); i ++)
	      {
		assert (4 + i * 4 + 1 < argc);
		a[i].start = atol (argv[4 + i * 4]);
		a[i].count = atol (argv[4 + i * 4 + 1]);
		a[i].startprev = atol (argv[4 + i * 4 + 2]);
		a[i].countprev = atol (argv[4 + i * 4 + 3]);
	      }

	    assert (4 + i * 4 == argc);

	    return 0;
	  }

	  /* Read the record if the last time.  */
	  sqlite3_exec_printf (access_db,
			       "select * from accesses where uid = %"PRId64";",
			       access_callback, NULL, &errmsg, uid);
	  if (errmsg)
	    {
	      debug (0, "%s: %s", filename, errmsg);
	      sqlite3_free (errmsg);
	      errmsg = NULL;
	    }

	  struct stat statbuf;
	  statbuf.st_size = 0;
	  if (stat (filename, &statbuf) == 0)
	    statbuf.st_size ++;

	  sqlite3_exec_printf
	    (access_db,
	     "insert into log values (%"PRId64", %"PRId64", %"PRId64");",
	     NULL, NULL, &errmsg,
	     uid, last_access, (uint64_t) statbuf.st_size);
	  if (errmsg)
	    {
	      debug (0, "%s: %s", filename, errmsg);
	      sqlite3_free (errmsg);
	      errmsg = NULL;
	    }


	  if (needs_update)
	    {
	      last_access = n;

	      uint64_t range = 24 * 60 * 60;
	      int i;
	      for (i = 0; i < sizeof (a) / sizeof (a[0]);
		   i ++, range *= 2)
		{
		  if (last_access > a[i].start + range)
		    {
		      a[i].startprev = a[i].start;
		      a[i].countprev = a[i].count;

		      a[i].start = last_access;
		      a[i].count = 1;
		    }
		  else
		    a[i].count ++;
		}

	      int err = sqlite3_exec_printf
		(access_db,
		 "insert or replace into accesses values"
		 " (%"PRId64", %"PRId64", %"PRId64", %"PRId64", "
		 "  %"PRId64", %d, %"PRId64", %d, "
		 "  %"PRId64", %d, %"PRId64", %d, "
		 "  %"PRId64", %d, %"PRId64", %d, "
		 "  %"PRId64", %d, %"PRId64", %d, "
		 "  %"PRId64", %d, %"PRId64", %d, "
		 "  %"PRId64", %d, %"PRId64", %d, "
		 "  %"PRId64", %d, %"PRId64", %d, "
		 "  %"PRId64", %d, %"PRId64", %d, "
		 "  %"PRId64", %d, %"PRId64", %d);",
		 NULL, NULL, &errmsg,
		 uid, created, last_access, (uint64_t) statbuf.st_size,
		 a[0].start, a[0].count, a[0].startprev, a[0].countprev,
		 a[1].start, a[1].count, a[1].startprev, a[1].countprev,
		 a[2].start, a[2].count, a[2].startprev, a[2].countprev,
		 a[3].start, a[3].count, a[3].startprev, a[3].countprev,
		 a[4].start, a[4].count, a[4].startprev, a[4].countprev,
		 a[5].start, a[5].count, a[5].startprev, a[5].countprev,
		 a[6].start, a[6].count, a[6].startprev, a[6].countprev,
		 a[7].start, a[7].count, a[7].startprev, a[7].countprev,
		 a[8].start, a[8].count, a[8].startprev, a[8].countprev);

	      if (err)
		sqlite3_exec (access_db, "rollback transaction;",
			      NULL, NULL, NULL);
	      if (errmsg)
		{
		  debug (0, "%s: %s", filename, errmsg);
		  sqlite3_free (errmsg);
		  errmsg = NULL;
		}
	    }

	  free (notice);
	  notice = next;
	}
      while (notice);

      debug (0, "Processed %d notices", processed);

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
   FILENAME.  */
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

      debug (0, "Processing %s", filename);

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
	    /* We can get IN_OPEN and IN_MODIFY events for files in
	       directories if we are only watching directories.  We
	       cannot get IN_ACCESS events in this case.  */
	    int watch = inotify_add_watch (inotify_fd, filename,
					   IN_CREATE | IN_DELETE
					   | IN_DELETE_SELF
					   | IN_OPEN | IN_MODIFY);
	    if (watch < 0)
	      switch (errno)
		{
		case EACCES:
		  return 0;
		default:
		  inotify_fd = -1;
		  error (1, errno, "inotify_add_watch (%s) -> %d (%d total)",
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

      debug (0, "Processed %s (%d watches)", filename, total_watches);

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
  pthread_t tid;
  int err = pthread_create (&tid, NULL, directory_add_helper, NULL);
  if (err < 0)
    error (1, errno, "pthread_create");
  pthread_detach (tid);

  err = pthread_create (&tid, NULL, notice_add_helper, NULL);
  if (err < 0)
    error (1, errno, "pthread_create");
  pthread_detach (tid);


  char buffer[16 * 4096];
  int have = 0;
  while (1)
    {
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

	  
	  char *events = inotify_mask_to_string (ev->mask);

	  struct watch_node *wn = btree_watch_find (&watch_btree, &ev->wd);
	  assert (wn);

	  char *element = ev->name;
	  if (ev->len == 0)
	    element = NULL;

	  char *filename = NULL;
	  asprintf (&filename, "%s%s%s%s%s",
		    base,
		    *wn->filename ? "/" : "", wn->filename,
		    element ? "/" : "", element ?: "");

	  debug (0, "%s: %s (%x)",
		 filename, events, ev->mask);
	  free (events);

	  if (! under_dot_dir (filename))
	    {
	      if ((ev->mask & IN_CREATE))
		directory_add (strdup (filename));

	      if ((ev->mask & IN_DELETE_SELF))
		{
		  debug (0, "Deleted: %s: %s (%x)",
			 filename, events, ev->mask);

		  inotify_rm_watch (inotify_fd, ev->wd);
		}

	      if ((ev->mask & IN_IGNORED))
		{
		  btree_watch_detach (&watch_btree, wn);
		  free (wn);
		  total_watches --;
		}

	      notice_add (filename, ev->mask);
	    }

	  free (filename);

	  len -= sizeof (*ev) + ev->len;
	  ev = (void *) ev + sizeof (*ev) + ev->len;
	}
    }

  return 0;
}
