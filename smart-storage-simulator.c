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
#include "util.h"

static uint64_t cache_space;

/* Files.  */
struct file
{
  uint64_t access_time;
  uint64_t size;

  struct
  {
    uint64_t start;
    int count;
    uint64_t startprev;
    int countprev;
  } accesses[9];

  /* This is used by the optimal policy.  */
  uint64_t next_access;

  struct list_node node;
  uint32_t hash;
  short filename_len;
  char filename[];
};

build_assert (offsetof (struct file, hash) + sizeof (uint32_t)
	      == offsetof (struct file, filename_len));
build_assert (offsetof (struct file, filename_len) + sizeof (short)
	      == offsetof (struct file, filename));

LIST_CLASS(file, struct file, node, true)

static uint64_t bytes_allocated;

struct file *
file_new (char *filename, uint64_t access_time, uint64_t size)
{
  int len = strlen (filename);
  int s = sizeof (struct file) + len + 1;
  bytes_allocated += s;
  struct file *file = calloc (s, 1);
  file->filename_len = len;
  memcpy (file->filename, filename, len + 1);

  /* This is a very simple hash.  It is based on the observation that
     filenames will tend to differ at the end.  */
  file->hash = 0;
  int i;
  for (i = 1; i <= 6; i ++)
    if (len >= i * sizeof (uint32_t))
      file->hash
	^= * (uint32_t *) &file->filename[len - i * sizeof (uint32_t)];

  file->access_time = access_time;
  file->size = size;
  return file;
}

static void
file_free (struct file *file)
{
  bytes_allocated -= sizeof (*file) + file->filename_len + 1;
  free (file);
}

static bool
file_same (struct file *a, struct file *b)
{
  return memcmp (&a->hash, &b->hash,
		 sizeof (a->hash) + sizeof (a->filename_len)
		 + a->filename_len) == 0;
}

static struct file *
file_list_find (struct file_list *list, char *filename)
{
  int l = strlen (filename);

  /* If we are adding it, it should not be on the list.  */
  struct file *f;
  for (f = file_list_head (list); f; f = file_list_next (f))
    if (l == f->filename_len && memcmp (f->filename, filename, l) == 0)
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
  file_free (f);

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
  /* Total bytes required to keep everything in the cache.  */
  uint64_t bytes_max;
  /* The number of bytes that had to be read from the network.  */
  uint64_t bytes_fetched;

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

  /* The sum of the times since the previous access.  */
  uint64_t iir;

  file_evict_t evict;
};

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
	  /* Only count at most one access per hour.  (We should have
	     filtered this out already.)  */
	  assert (access_time - file->access_time >= 60 * 60);

	  file_list_unlink (&status->files, file);

	  growth = size - file->size;
	  file->size = size;

	  status->hits ++;
	  status->iir += access_time - file->access_time;
	  status->hits_bytes += size;

	  debug (0, "%s (change: "BYTES_FMT", cache: "BYTES_FMT"): hit!"
		 " (last access: "TIME_FMT")",
		 filename, BYTES_PRINTF (growth),
		 BYTES_PRINTF (status->bytes_count),
		 TIME_PRINTF (1000 * (access_time - file->access_time)));

	  file->access_time = access_time;

	  status->bytes_max += growth;
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
	      file->size = size;
	      status->misses ++;

	      status->bytes_max += size - file->size;
	      status->iir += access_time - file->access_time;
	      file->access_time = access_time;
	    }
	  else
	    /* File is new.  */
	    {
	      real_miss = false;
	      file = file_new (filename, access_time, size);
	      status->files_total ++;

	      status->bytes_max += size;
	    }

	  growth = size;

	  status->file_count ++;

	  status->bytes_fetched += size;

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
      while (status->bytes_count > cache_space)
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

      file_list_enqueue (&status->files, file);

      uint64_t t = 0;
      for (file = file_list_head (&status->files);
	   file;
	   file = file_list_next (file))
	t += file->size;
      assertx (t == status->bytes_count,
	       "%"PRId64" != %"PRId64,
	       t, status->bytes_count);
    }
}

int
main (int argc, char *argv[])
{
  void usage (int status)
  {
    fprintf (stderr,
	     "Usage: %s CACHE_SIZE ACCESS_DB [PREFIX...]\n", argv[0]);
    exit (status);
  }

  if (argc <= 2)
    {
      usage (1);
    }

  char *tail = NULL;
  cache_space = strtol (argv[1], &tail, 10);
  if (cache_space == 0)
    {
      fprintf (stderr, "Cache space must be >0.\n");
      usage (1);
    }
  if (tail)
    switch (*tail)
      {
      case 'G': case 'g':
	cache_space *= 1024 * 1024 * 1024;
	break;
      case 'M': case 'm':
	cache_space *= 1024 * 1024;
	break;
      case 'K': case 'k':
	cache_space *= 1024;
	break;
      default:
	;
      }
  debug (0, "Cache size set to "BYTES_FMT, BYTES_PRINTF (cache_space));

  char *filename = argv[2];
  sqlite3 *access_db;
  int err = sqlite3_open (filename, &access_db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (access_db));
  sqlite3_busy_timeout (access_db, 60 * 60 * 1000);

  char *errmsg = NULL;
  sqlite3_exec (access_db,
		"PRAGMA legacy_file_format = false;"
		"create index if not exists time on log (time);",
		NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%s: %s", filename, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

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

	      assert (start <= status->access_time);
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
	  if (file_same (f, l))
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

  int prefix_len = 0;
  int record_count = 0;
  int callback (void *cookie, int cargc, char **cargv, char **names)
  {
    assert (cargc == 4);

    // int uid = atol (cargv[0]);
    uint64_t access_time = atol (cargv[1]);
    uint64_t size_plus_one = atol (cargv[2]);
    char *filename = cargv[3];

    file_list_enqueue (&access_log, file_new (filename + prefix_len,
					      access_time,
					      size_plus_one));
    record_count ++;

    return 0;
  }

  uint64_t start = now ();

  char q[16 * 1024];
  q[0] = 0;
  if (argc > 2)
    {
      char *p = q;
      void append (const char *s)
      {
	int l = strlen (s);
	if ((uintptr_t) p - (uintptr_t) q + l >= sizeof (q))
	  error (ENOMEM, 1, "Out of memory.");
	memcpy (p, s, l);
	p += l;
      }

      append ("where ");
      int i;
      int shortest = 0;
      for (i = 3; i < argc; i ++)
	{
	  if (i > 3)
	    append (" or ");

	  append ("filename like '");
	  append (argv[i]);
	  append ("%'");

	  int l = strlen (argv[i]);
	  if (i == 3)
	    shortest = l;
	  else
	    shortest = MIN (l, shortest);
	}
      append (" ");
      *p = 0;
      debug (0, "--%s--", q);

      /* Find the common prefix.  */
      for (prefix_len = 0; prefix_len < shortest; prefix_len ++)
	for (i = 4; i < argc; i ++)
	  if (argv[i][prefix_len] != argv[3][prefix_len])
	    break;

      debug (0, "prefix: %*s (%d)", prefix_len, argv[3], prefix_len);
    }

  sqlite3_exec_printf
    (access_db,
     "select uid, time, size_plus_one, filename from log "
     " join (select uid, filename from files) "
     " using (uid) %s"
     "order by time;",
     callback, NULL, &errmsg, q);
  if (errmsg)
    {
      debug (0, "%s: %s", filename, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  uint64_t end = now ();
  debug (0, "%d records ("BYTES_FMT" bytes) ("TIME_FMT")",
	 record_count, BYTES_PRINTF (bytes_allocated),
	 TIME_PRINTF (end - start));
  start = end;

  int processed = 0;
  uint64_t last = end;
  int records = record_count;
  struct file *f;
  /* Compress records such that there is at most one record for any 60
     minutes period.  Use the time from the earliest access and the
     size from the last access.  */
  for (f = file_list_head (&access_log); f; f = file_list_next (f))
    {
      struct file *next = file_list_next (f);
      struct file *q;
      while ((q = next) && q->access_time - f->access_time < 60 * 60)
	{
	  next = file_list_next (q);

	  if (file_same (f, q))
	    {
	      /* Take the size at the end.  */
	      f->size = q->size;
	      file_list_unlink (&access_log, q);
	      file_free (q);
	      record_count --;

	      processed ++;
	    }
	}

      processed ++;
      if (now () - last > 5000)
	{
	  end = now ();
	  last = end;
	  debug (0, "Processed %d records %d%%, deleted %d ("TIME_FMT")",
		 processed, (100 * processed) / records,
		 records - record_count,
		 TIME_PRINTF (end - start));
	}
    }

  end = now ();
  debug (0, "%d records temporal after compression ("TIME_FMT")",
	 record_count, TIME_PRINTF (end - start));
  start = end;

  struct file_list files;
  memset (&files, 0, sizeof (files));
  int file_count = 0;

  int r = 0;
  for (f = file_list_head (&access_log); f; f = file_list_next (f))
    {
      r ++;
      if (! file_list_find (&files, f->filename))
	{
	  file_list_push (&files, file_new (f->filename, 0, 0));
	  file_count ++;
	  if (r % 1000 == 0)
	    debug (0, "%d files, %d/%d records", file_count, r, record_count);
	}
    }

  end = now ();
  debug (0, "Identified %d unique files ("TIME_FMT")",
	 r, TIME_PRINTF (end - start));
  start = end;


  /* Prune any directories.  */
  struct file *next = file_list_head (&access_log);
  while ((f = next))
    {
      next = file_list_next (f);

      assert (f->filename);
      int len = strlen (f->filename);
      assert (len > 0);

      bool is_directory = false;
      struct file *q;
      for (q = file_list_head (&files); q; q = file_list_next (q))
	if (strncmp (f->filename, q->filename, len) == 0
	    && q->filename[len] == '/')
	  {
	    is_directory = true;
	    break;
	  }
      
      if (is_directory || f->size == 1 || f->size == 4097)
	{
	  file_list_unlink (&access_log, f);
	  file_free (f);
	  record_count --;
	}
    }

  end = now ();
  debug (0, "%d files, %d records, after directory pruning ("TIME_FMT")",
	 file_count, record_count, TIME_PRINTF (end - start));
  start = end;


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

  printf ("%d files: "BYTES_FMT"\n",
	  opt.files_total, BYTES_PRINTF (opt.bytes_max));
  printf ("OPT performance: "BYTES_FMT", %d hits ("TIME_FMT"), "
	  "%d misses (subsequent)\n",
	  BYTES_PRINTF (opt.bytes_fetched),
	  opt.hits, TIME_PRINTF (1000 * opt.iir / (opt.hits + opt.misses)),
	  opt.misses);
  printf ("LRU performance: "BYTES_FMT", %d hits ("TIME_FMT"), "
	  "%d misses (subsequent)\n",
	  BYTES_PRINTF (lru.bytes_fetched),
	  lru.hits, TIME_PRINTF (1000 * lru.iir / (lru.hits + lru.misses)),
	  lru.misses);
  printf ("LFU performance: "BYTES_FMT", %d hits  ("TIME_FMT"), "
	  "%d misses (subsequent)\n",
	  BYTES_PRINTF (lfu.bytes_fetched),
	  lfu.hits, TIME_PRINTF (1000 * lfu.iir / (lfu.hits + lfu.misses)),
	  lfu.misses);

  return 0;
}
