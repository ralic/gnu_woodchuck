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

  union
  {
    struct
    {
      /* The time of the following access.  */
      uint64_t next_access;
    } opt;
    struct
    {
      /* ARC maintains four lists:

	   - objects used once that are in memory (t1)
	   - objects used once and have been recently evicted (b1)
	   - objects that have been used more than once and are in memory (t2)
	   - objects used more and have been recently evicted (b2).  */
      enum
	{
	  arc_none = 0,
	  arc_t1,
	  arc_b1,
	  arc_t2,
	  arc_b2,
	} status;
      struct list_node node;
    } arc;
  };

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
  struct file *file = malloc (s);
  memset (file, 0, sizeof (*file));
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

LIST_CLASS(l1l2, struct file, arc.node, true)

static struct file *
l1l2_list_find (struct l1l2_list *list, char *filename)
{
  int l = strlen (filename);

  /* If we are adding it, it should not be on the list.  */
  struct file *f;
  for (f = l1l2_list_head (list); f; f = l1l2_list_next (f))
    if (l == f->filename_len && memcmp (f->filename, filename, l) == 0)
      return f;
  return NULL;
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

  /* Policy specific data.  */
  enum
    {
      policy_opt,
      policy_lru,
      policy_lfu,
      policy_arc,
    } policy;
  union
  {
    struct
    {
      struct l1l2_list t1;
      uint64_t t1_size;
      struct l1l2_list b1;
      uint64_t b1_size;
      struct l1l2_list t2;
      uint64_t t2_size;
      struct l1l2_list b2;
      uint64_t b2_size;
      uint64_t p;
    } arc;
  };

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

  /* The eviction routine.  */
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
      uint64_t size_prev = 0;
      struct file *file = file_list_find (&status->files, filename);
      if (file)
	/* This is an access.  Add the file to the end of the
	   access list.  */
	{
	  /* Only count at most one access per hour.  (We should have
	     filtered this out already.)  */
	  assert (access_time - file->access_time >= 60 * 60);
	  growth = size - file->size;

	  file_list_unlink (&status->files, file);

	  size_prev = file->size;
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

	  if (status->policy == policy_arc)
	    assert (file->arc.status == arc_t1 || file->arc.status == arc_t2);
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
	      size_prev = file->size;
	      file->size = size;
	      status->misses ++;

	      status->bytes_max += size - file->size;
	      status->iir += access_time - file->access_time;
	      file->access_time = access_time;

	      if (status->policy == policy_arc)
		assert (file->arc.status == arc_none
			|| file->arc.status == arc_b1
			|| file->arc.status == arc_b2);
	    }
	  else
	    /* File is new.  */
	    {
	      real_miss = false;
	      file = file_new (filename, access_time, size);
	      status->files_total ++;

	      status->bytes_max += size;

	      if (status->policy == policy_arc)
		assert (file->arc.status == arc_none);
	    }

	  growth = size;

	  status->file_count ++;

	  status->bytes_fetched += size;

	  debug (0, "%s (size: "BYTES_FMT", cache: "BYTES_FMT", %d): %s!",
		 filename, BYTES_PRINTF (growth),
		 BYTES_PRINTF (status->bytes_count), status->file_count,
		 real_miss ? "miss" : "new");
	}

      if (status->policy == policy_arc)
	/* Remove the file from its current list and update its
	   status.  Do not yet add it to its new list: it must not be
	   evicted immediately.  */
	switch (file->arc.status)
	  {
	  case arc_none:
	    file->arc.status = arc_t1;
	    break;

	  case arc_t1:
	    assert (l1l2_list_find (&status->arc.t1, file->filename));
	    l1l2_list_unlink (&status->arc.t1, file);
	    status->arc.t1_size -= size_prev;

	    file->arc.status = arc_t2;
	    break;

	  case arc_b1:
	    assert (l1l2_list_find (&status->arc.b1, file->filename));
	    l1l2_list_unlink (&status->arc.b1, file);
	    status->arc.b1_size -= size_prev;

	    file->arc.status = arc_t2;

	    status->arc.p += size;
	    if (status->arc.p > cache_space)
	      status->arc.p = cache_space;
		
	    break;

	  case arc_t2:
	    assert (l1l2_list_find (&status->arc.t2, file->filename));
	    l1l2_list_unlink (&status->arc.t2, file);
	    status->arc.t2_size -= size_prev;

	    file->arc.status = arc_t2;

	    break;

	  case arc_b2:
	    assert (l1l2_list_find (&status->arc.b2, file->filename));
	    l1l2_list_unlink (&status->arc.b2, file);
	    status->arc.b2_size -= size_prev;

	    file->arc.status = arc_t2;

	    if (status->arc.p >= size)
	      status->arc.p -= size;
	    else 
	      status->arc.p = 0;

	    break;
	  default:
	    assertx (0, "Bad arc status: %d", file->arc.status);
	    abort ();
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
      while (status->bytes_count + growth > cache_space)
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
	  assert (loser != file);
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

	  if (status->policy == policy_arc)
	    switch (loser->arc.status)
	      {
	      case arc_t1:
		assert (l1l2_list_find (&status->arc.t1, loser->filename));
		l1l2_list_unlink (&status->arc.t1, loser);
		status->arc.t1_size -= loser->size;

		l1l2_list_push (&status->arc.b1, loser);
		status->arc.b1_size += loser->size;

		loser->arc.status = arc_b1;
		break;

	      case arc_t2:
		assert (l1l2_list_find (&status->arc.t2, loser->filename));
		l1l2_list_unlink (&status->arc.t2, loser);
		status->arc.t2_size -= loser->size;

		l1l2_list_push (&status->arc.b2, loser);
		status->arc.b2_size += loser->size;

		loser->arc.status = arc_b2;
		break;

	      default:
		assertx (0, "Bad arc status: %d", loser->arc.status);
		abort ();
	      }
	}

      if (growth < 0)
	assert (status->bytes_count >= - growth);
      status->bytes_count += growth;

      file_list_enqueue (&status->files, file);

      if (status->policy == policy_arc)
	{
	  switch (file->arc.status)
	    {
	    case arc_t1:
	      l1l2_list_push (&status->arc.t1, file);
	      status->arc.t1_size += size;
	      break;

	    case arc_t2:
	      l1l2_list_push (&status->arc.t2, file);
	      status->arc.t2_size += size;
	      break;

	    default:
	      assertx (0, "Bad arc status: %d", file->arc.status);
	      abort ();
	    }

	  assertx (status->arc.t1_size + status->arc.t2_size
		   == status->bytes_count,
		   "%"PRId64"+%"PRId64"=%"PRId64" ?= %"PRId64,
		   status->arc.t1_size, status->arc.t2_size,
		   status->arc.t1_size + status->arc.t2_size,
		   status->bytes_count);

	  /* Remove entries from the bottom lists: neither list should
	     contain more than CACHE_SPACE bytes.  */
	  assertx (status->arc.t1_size <= cache_space,
		   BYTES_FMT"/"BYTES_FMT" vs. "BYTES_FMT,
		   BYTES_PRINTF (status->arc.t1_size),
		   BYTES_PRINTF (status->arc.t2_size),
		   BYTES_PRINTF (cache_space));
	  while (status->arc.t1_size + status->arc.b1_size > cache_space)
	    {
	      struct file *loser = l1l2_list_dequeue (&status->arc.b1);
	      assert (loser->arc.status == arc_b1);
	      loser->arc.status = arc_none;
	      status->arc.b1_size -= loser->size;
	    }
	  assert (status->arc.t2_size <= cache_space);
	  while (status->arc.t2_size + status->arc.b2_size > cache_space)
	    {
	      struct file *loser = l1l2_list_dequeue (&status->arc.b2);
	      assert (loser->arc.status == arc_b2);
	      loser->arc.status = arc_none;
	      status->arc.b2_size -= loser->size;
	    }
	}

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


  struct file *optimal_evict (struct status *status)
  {
    struct file *f;
    for (f = file_list_head (&status->files); f; f = file_list_next (f))
      {
	struct file *l;
	for (l = access_log_entry; l; l = file_list_next (l))
	  if (file_same (f, l))
	    {
	      f->opt.next_access = l->access_time;
	      break;
	    }
	if (! l)
	  f->opt.next_access = -1;
      }

    struct file *loser = NULL;
    for (f = file_list_head (&status->files); f; f = file_list_next (f))
      if (! loser || loser->opt.next_access < f->opt.next_access)
	{
	  loser = f;
	  if (f->opt.next_access == -1)
	    break;
	}

    return loser;
  }

  struct status opt;
  memset (&opt, 0, sizeof (opt));
  opt.policy = policy_opt;
  opt.evict = optimal_evict;


  struct file *lru_evict (struct status *status)
  {
    return file_list_head (&status->files);
  }

  struct status lru;
  memset (&lru, 0, sizeof (lru));
  lru.policy = policy_lru;
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
	debug (0, "Considering %s", f->filename);
	double score = 0;

	uint64_t range = 24 * 60 * 60;
	int i;
	for (i = 0; i < sizeof (f->accesses) / sizeof (f->accesses[0]);
	     i ++, range *= 2)
	  {
	    void process (uint64_t start, uint64_t length, double accesses)
	    {
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
		  double o = overlap (f->accesses[i - 1].start, range / 2,
				      start, range);
		  double b = accesses;
		  double d = f->accesses[i - 1].count * o;
		  accesses -= d;
		  if (o > 0.0001)
		    debug (5, "Overlap: %.2lf with %d: %.1lf - %.1lf -> %.1lf",
			   o, f->accesses[i - 1].count, b, d, accesses);

		  o = overlap (f->accesses[i - 1].startprev, range / 2,
			       start, range);
		  b = accesses;
		  d = f->accesses[i - 1].countprev * o;
		  accesses -= d;
		  if (o > 0.0001)
		    debug (5, "Overlap: %.2lf with %d: %.1lf - %.1lf -> %.1lf",
			   o, f->accesses[i - 1].countprev, b, d, accesses);

		  if (accesses < 0)
		    {
		      debug (0, "accesses dropped below 0: %0.1lf.",
			     accesses);
		      accesses = 0;
		    }
		}

	      double s = score;

	      double factor = 1.0;
	      if (accesses > 0)
		factor = MAX (1.0, log2 ((double) delta / (20 * 60 * 60)));
	      score += accesses / factor;

	      if (accesses > 0)
		debug (0, "Period %d: "TIME_FMT"+"TIME_FMT" ("TIME_FMT"): "
		       "accesses: %.1lf / %.1lf -> %.1lf; "
		       "score: %.1lf -> %.1lf",
		       i, TIME_PRINTF (1000 * (status->access_time - end)),
		       TIME_PRINTF (1000 * length), TIME_PRINTF (1000 * delta),
		       accesses, factor, accesses / factor, s, score);
	    }

	    process (f->accesses[i].start, range, f->accesses[i].count);
	    process (f->accesses[i].startprev, range,
		     f->accesses[i].countprev);
	  }

	debug (0, DEBUG_BOLD ("%s's score: %.1lf"), f->filename, score);

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
  lfu.policy = policy_lfu;
  lfu.evict = lfu_evict;


  struct file *arc_evict (struct status *status)
  {
    if (status->arc.t1_size > status->arc.p)
      return l1l2_list_head (&status->arc.t1);
    else
      {
	struct file *loser;
	loser = l1l2_list_head (&status->arc.t2);
	if (! loser)
	  loser = l1l2_list_head (&status->arc.t1);
	return loser;
      }
  }

  struct status arc;
  memset (&arc, 0, sizeof (arc));
  arc.policy = policy_arc;
  arc.evict = arc_evict;


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


  void run (struct status *status)
  {
    for (access_log_entry = file_list_head (&access_log);
	 access_log_entry;
	 access_log_entry = file_list_next (access_log_entry))
      access_notice (status, access_log_entry->filename,
		     access_log_entry->access_time, access_log_entry->size);
  }

  run (&opt);
  run (&lru);
  run (&lfu);
  run (&arc);

  void print (const char *name, struct status *status)
  {
    printf ("%s performance: "BYTES_FMT", %d hits ("TIME_FMT"), "
	    "%d misses (subsequent)\n",
	    name, BYTES_PRINTF (status->bytes_fetched),
	    status->hits,
	    TIME_PRINTF (status->hits + status->misses > 0
			 ? (1000 * status->iir
			    / (status->hits + status->misses))
			 : 0),
	    status->misses);
  }

  printf ("%d files: "BYTES_FMT"\n",
	  opt.files_total, BYTES_PRINTF (opt.bytes_max));
  print ("OPT", &opt);
  print ("LRU", &lru);
  print ("LFU", &lfu);
  printf ("ARC parameter: "BYTES_FMT" (%"PRId64"%%)\n",
	  BYTES_PRINTF (arc.arc.p), (100 * arc.arc.p) / cache_space);
  print ("ARC", &arc);

  return 0;
}
