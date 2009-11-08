/* smart-storage-simulator.c - Smart storage policy simulator.
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
#include <math.h>
#include <sqlite3.h>

#include "debug.h"
#include "list.h"
#include "btree.h"
#include "util.h"

/* Files.  */
struct file
{
  char *filename;
  uint64_t access_time;
  uint64_t size;
  struct list_node node;

  struct
  {
    uint64_t start;
    int count;
    uint64_t startprev;
    int countprev;
  } accesses[9];

  /* This is used by the optimal policy.  */
  uint64_t next_access;
};

LIST_CLASS(file, struct file, node, true)

struct file *
file_new (char *filename, uint64_t access_time, uint64_t size)
{
  struct file *file = calloc (sizeof (*file), 1);
  file->filename = strdup (filename);
  file->access_time = access_time;
  file->size = size;
  return file;
}

static struct file *
file_list_find (struct file_list *list, char *filename)
{
  /* If we are adding it, it should not be on the list.  */
  struct file *f;
  for (f = file_list_head (list); f; f = file_list_next (f))
    if (strcmp (f->filename, filename) == 0)
      return f;
  return NULL;
}

/* Returns true if the file was removed from the list, false
   otherwise.  */
static bool
file_list_remove (struct file_list *list, char *filename)
{
  struct file *f = file_list_find (list, filename);
  if (! f)
    return false;

  file_list_unlink (list, f);
  free (f);

  return true;
}

/* The access log.  */
struct file_list access_log;
struct file *access_log_entry;

struct status;

typedef struct file *(*file_evict_t) (struct status *status);

struct status
{
  struct file_list evictions;
  /* Files in the cache.  Maintained in LRU order.  */
  struct file_list files;

  /* Bytes in the cache.  */
  uint64_t bytes_count;
  /* Total bytes.  */
  uint64_t bytes_total;

  /* Number of files in the cache.  */
  int file_count;
  /* The number of unique files that were ever in the cache.  */
  int files_total;

  /* Cache hits and misses.  */
  int hits;
  int hits_bytes;
  int misses;

  /* Time of the last access.  */
  uint64_t access_time;

  file_evict_t evict;
};

/* Total space available.  */
#define SPACE (1ULL * 1024 * 1024 * 1024)
/* Keep about 1 GB free.  */
#define SPACE_LOW_WATER (1ULL * 1024 * 1024 * 1024)
/* Free until there are about 3 GB free.  */
#define SPACE_HIGH_WATER (3ULL * 1024 * 1024 * 1024)

static void
access_notice (struct status *status,
	       char *filename, uint64_t access_time, uint64_t size_plus_one)
{
  if (size_plus_one == 0)
    /* Deleted.  */
    {
    }
  else
    {
      uint64_t size = size_plus_one - 1;

      /* The number of bytes this file has grown by (possible
	 negative).  */
      int64_t growth;
      struct file *file = file_list_find (&status->files, filename);
      if (file)
	/* This is an access.  Add the file to the end of the
	   access list.  */
	{
	  if (access_time - file->access_time < 60 * 60)
	    /* Only count at most one access per hour.  */
	    return;

	  file_list_unlink (&status->files, file);

	  growth = size - file->size;
	  file->size = size;

	  status->hits ++;
	  status->hits_bytes += size;

	  debug (0, "%s (change: "BYTES_FMT", cache: "BYTES_FMT"): hit!"
		 " (last access: "TIME_FMT")",
		 filename, BYTES_PRINTF (growth),
		 BYTES_PRINTF (status->bytes_count),
		 TIME_PRINTF (1000 * (access_time - file->access_time)));

	  file->access_time = access_time;
	}
      else
	{
	  file = file_list_find (&status->evictions, filename);
	  bool real_miss;
	  if (file)
	    /* The file was previous evicted.  */
	    {
	      real_miss = true;
	      file_list_unlink (&status->evictions, file);
	      status->misses ++;
	    }
	  else
	    {
	      real_miss = false;
	      file = file_new (filename, access_time, size);
	      status->files_total ++;
	    }

	  growth = size;

	  status->file_count ++;

	  debug (0, "%s (size: "BYTES_FMT", cache: "BYTES_FMT", %d): %s!",
		 filename, BYTES_PRINTF (growth),
		 BYTES_PRINTF (status->bytes_count), status->file_count,
		 real_miss ? "miss" : "new");
	}

      /* Update the access stats.  */
      uint64_t range = 24 * 60 * 60;
      int i;
      for (i = 0; i < sizeof (file->accesses) / sizeof (file->accesses[0]);
	   i ++, range *= 2)
	{
	  if (access_time > file->accesses[i].start + range)
	    {
	      file->accesses[i].startprev = file->accesses[i].start;
	      file->accesses[i].countprev = file->accesses[i].count;

	      file->accesses[i].start = access_time;
	      file->accesses[i].count = 1;
	    }
	  else
	    file->accesses[i].count ++;
	}

      assert (access_time >= status->access_time);
      status->access_time = access_time;

      /* See if we need to free something.  */
      while (status->bytes_count > SPACE)
	/* Free something.  */
	{
	  struct file *loser = status->evict (status);
	  if (! loser)
	    {
	      debug (0, "Failed to evict something (adding: %s, "BYTES_FMT"): "
		     BYTES_FMT", count: %d",
		     filename, BYTES_PRINTF (size),
		     BYTES_PRINTF (status->bytes_count),
		     status->file_count - 1);
	    }
	  assert (loser);
	  file_list_unlink (&status->files, loser);

	  debug (0, "%s (size: "BYTES_FMT"): evicted!"
		 " cache: "BYTES_FMT"->"BYTES_FMT", count: %d",
		 loser->filename, BYTES_PRINTF (loser->size),
		 BYTES_PRINTF (status->bytes_count),
		 BYTES_PRINTF (status->bytes_count - loser->size),
		 status->file_count - 2);

	  status->file_count --;
	  status->bytes_count -= loser->size;
	  file_list_enqueue (&status->evictions, loser);
	}

      if (growth < 0)
	assert (status->bytes_count >= - growth);
      status->bytes_count += growth;
      status->bytes_total += growth;

      file_list_enqueue (&status->files, file);
    }
}

int
main (int argc, char *argv[])
{
  void usage (int status)
  {
    fprintf (stderr,
	     "Usage: %s ACCESS_DB [PREFIX...]\n", argv[0]);
    exit (status);
  }

  if (argc == 1)
    {
      usage (1);
    }

  char *filename = argv[1];
  sqlite3 *access_db;
  int err = sqlite3_open (filename, &access_db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (access_db));
  sqlite3_busy_timeout (access_db, 60 * 60 * 1000);

  struct file *lru_evict (struct status *status)
  {
    return file_list_head (&status->files);
  }

  struct status lru;
  memset (&lru, 0, sizeof (lru));
  lru.evict = lru_evict;

  struct file *lfu_evict (struct status *status)
  {
    /* Returns the amount by which the first region overlaps with the
       second.  (From 0.0 to 1.0.)  */
    double overlap (uint64_t start1, uint64_t length1,
		    uint64_t start2, uint64_t length2)
    {
      uint64_t end1 = MIN (start1 + length1 - 1, status->access_time);
      uint64_t end2 = MIN (start2 + length2 - 1, status->access_time);

      if (start2 <= start1 && start1 <= end2)
	/* START1 falls between the second region.  */
	{
	  if (start2 <= end1 && end1 <= end2)
	    /* END1 does as well.  Thus, overlap is 100%  */
	    return 1.0;
	  else
	    /* The start of the first region overlaps with the end of
	       the second.  */
	    return ((double) (end2 - start1 + 1)) / length1;
	}
      else if (start2 <= end1 && end1 <= end2)
	/* START1 does not fall between the second region but END1
	   does.  The end of the first region overlaps with the start
	   of the second.  */
	return ((double) (end1 - start2 + 1)) / length1;
      else
	return 0.0;
    }

    struct file *loser = NULL;
    double loser_score = 0;

    struct file *f;
    for (f = file_list_head (&status->files); f; f = file_list_next (f))
      {
	debug (5, "Considering %s", f->filename);
	double score = 0;

	uint64_t range = 24 * 60 * 60;
	int i;
	for (i = 0; i < sizeof (f->accesses) / sizeof (f->accesses[0]);
	     i ++, range *= 2)
	  {
	    void process (uint64_t start, uint64_t length, double accesses)
	    {
	      double a = accesses;

	      if (! accesses)
		return;

	      assert (start < status->access_time);
	      uint64_t end = MIN (start + length - 1, status->access_time);
	      length = end - start + 1;

	      uint64_t mid = (start / 2) + (end / 2);
	      uint64_t delta = status->access_time - mid;
	      assertx (status->access_time >= mid,
		       "(%"PRId64"-%"PRId64")/2 -> %"PRId64,
		       start, end, delta);

	      if (i > 0)
		/* Ignore accesses we've already counted.  */
		{
		  accesses -= f->accesses[i - 1].count
		    * overlap (f->accesses[i - 1].start, range / 2,
			       start, range);

		  accesses -= f->accesses[i - 1].countprev
		    * overlap (f->accesses[i - 1].startprev, range / 2,
			       start, range);
		}

	      double s = score;

	      double factor = 1.0;
	      if (accesses > 0)
		factor = MAX (1.0, log2 ((double) delta / (20 * 60 * 60)));
	      score += accesses / factor;

	      debug (5, "Period %d: "TIME_FMT"+"TIME_FMT" ("TIME_FMT"): "
		     "accesses: %.1lf -> %.1lf; "
		     "score: %.1lf -> %.1lf (%"PRId64"->%.1lf)",
		     i, TIME_PRINTF (1000 * (status->access_time - end)),
		     TIME_PRINTF (1000 * length), TIME_PRINTF (1000 * delta),
		     a, accesses, s, score, delta, factor);
	    }

	    process (f->accesses[i].start, range, f->accesses[i].count);
	    process (f->accesses[i].startprev, range, f->accesses[i].countprev);
	  }

	if (! loser || score < loser_score)
	  {
	    loser = f;
	    loser_score = score;
	  }
      }

    return loser;
  }

  struct status lfu;
  memset (&lfu, 0, sizeof (lfu));
  lfu.evict = lfu_evict;

  struct file *optimal_evict (struct status *status)
  {
    struct file *f;
    for (f = file_list_head (&status->files); f; f = file_list_next (f))
      {
	struct file *l;
	for (l = access_log_entry; l; l = file_list_next (l))
	  if (strcmp (f->filename, l->filename) == 0)
	    {
	      f->next_access = l->access_time;
	      break;
	    }
	if (! l)
	  f->next_access = -1;
      }

    struct file *loser = NULL;
    for (f = file_list_head (&status->files); f; f = file_list_next (f))
      if (! loser || loser->next_access < f->next_access)
	{
	  loser = f;
	  if (f->next_access == -1)
	    break;
	}

    return loser;
  }

  struct status opt;
  memset (&opt, 0, sizeof (opt));
  opt.evict = optimal_evict;

  int callback (void *cookie, int cargc, char **cargv, char **names)
  {
    assert (cargc == 4);

    // int uid = atol (cargv[0]);
    uint64_t access_time = atol (cargv[1]);
    uint64_t size_plus_one = atol (cargv[2]);
    char *filename = cargv[3];

    int i;
    for (i = 2; i < argc; i ++)
      if (strncmp (argv[i], filename, strlen (argv[i])) == 0)
	break;

    if (i != argc)
      file_list_enqueue (&access_log, file_new (filename, access_time,
						size_plus_one));

    return 0;
  }

  char *errmsg = NULL;
  sqlite3_exec (access_db,
		"select uid, time, size_plus_one, filename from log "
		" join (select uid, filename from files) "
		" using (uid) "
		"order by time;",
		callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%s: %s", filename, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  /* Prune any directories.  */
  struct file *f;
  struct file *next = file_list_head (&access_log);
  while ((f = next))
    {
      next = file_list_next (f);

      bool is_directory = false;
      int callback (void *cookie, int cargc, char **cargv, char **names)
      {
	is_directory = true;
	return 0;
      }

      sqlite3_exec_printf
	(access_db,
	 "select * from files where filename like '%q/%%' limit 1",
	 callback, NULL, &errmsg, f->filename);
      if (errmsg)
	{
	  debug (0, "%s: %s", filename, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	}
      if (is_directory || f->size == 1 || f->size == 4097)
	{
	  file_list_unlink (&access_log, f);
	  free (f);
	}
    }


  for (access_log_entry = file_list_head (&access_log);
       access_log_entry;
       access_log_entry = file_list_next (access_log_entry))
    access_notice (&opt, access_log_entry->filename,
		   access_log_entry->access_time, access_log_entry->size);

  for (access_log_entry = file_list_head (&access_log);
       access_log_entry;
       access_log_entry = file_list_next (access_log_entry))
    access_notice (&lru, access_log_entry->filename,
		   access_log_entry->access_time, access_log_entry->size);

  for (access_log_entry = file_list_head (&access_log);
       access_log_entry;
       access_log_entry = file_list_next (access_log_entry))
    access_notice (&lfu, access_log_entry->filename,
		   access_log_entry->access_time, access_log_entry->size);

  printf ("OPT performance: %d files ("BYTES_FMT"), %d hits, %d misses (subsequent)\n",
	  opt.files_total, BYTES_PRINTF (opt.bytes_total),
	  opt.hits, opt.misses);
  printf ("LRU performance: %d files ("BYTES_FMT"), %d hits, %d misses (subsequent)\n",
	  lru.files_total, BYTES_PRINTF (lru.bytes_total),
	  lru.hits, lru.misses);
  printf ("LFU performance: %d files ("BYTES_FMT"), %d hits, %d misses (subsequent)\n",
	  lfu.files_total, BYTES_PRINTF (lfu.bytes_total),
	  lfu.hits, lfu.misses);

  return 0;
}
