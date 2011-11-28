/* debug.c - Debugging tools.
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
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.  */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <error.h>
#include <stdarg.h>

#include "debug.h"

#ifdef LOG_TO_DB
#include "util.h"
#include "files.h"
#include <sqlite3.h>
#include <sqlq.h>
#include <pthread.h>

static __thread sqlite3 *debug_output_file;
static char *debug_output_filename;
static __thread struct sqlq *debug_output_buffer;

#endif

#if !defined(DEBUG_ELIDE)
int output_debug_global = 3;
__thread int output_debug = 3;
#endif

#define DEBUG_STDERR(function, line, return_address, msg)	\
  ({								\
    time_t __t = time (NULL);					\
    struct tm __tm;						\
    localtime_r (&__t, &__tm);					\
    								\
    fprintf (stderr, "%d.%d.%d %d:%02d.%02d:%s:%d:(%p): %s\n",		\
	     1900 + __tm.tm_year, __tm.tm_mon + 1, __tm.tm_mday,	\
	     __tm.tm_hour, __tm.tm_min, __tm.tm_sec,			\
	     function, line, return_address, msg);			\
    fflush (stderr);							\
  })

#ifdef LOG_TO_DB
static void
sqlq_error_handler (const char *file, const char *func, int line,
		    const char *sql, const char *error_message)
{
  char *msg = NULL;
  asprintf (&msg, "%s:%s:%d: Executing sql '%s': %s\n",
	    file, func, line, sql, error_message);

  static __thread bool in;
  if (! in)
    /* Try to use debug, but don't end up in an infinite loop.  */
    {
      in = true;
      debug (0, msg);
      in = false;
    }

  DEBUG_STDERR (__func__, __LINE__, __builtin_return_address (0), msg);
}
#endif

void
debug_ (const char *file, const char *function, int line,
	void *return_address, int level,
	bool async, const char *fmt, ...)
{
  debug_init_ ();

  va_list ap;
  va_start (ap, fmt);

  char *msg = NULL;
  vasprintf (&msg, fmt, ap);

#ifdef LOG_TO_DB
  uint64_t n = now ();
  static uint64_t last_tz_check;
  static int tz;
  if (last_tz_check - n > 24 * 60 * 60 * 1000)
    {
      time_t t = n / 1000;
      struct tm local;
      localtime_r (&t, &local);
      struct tm utc;
      gmtime_r (&t, &utc);

      if (local.tm_hour < utc.tm_hour)
	local.tm_hour += 24;
      tz = (local.tm_hour * 60 + local.tm_min)
	- (utc.tm_hour * 60 + utc.tm_min);
    }

  char *sql = sqlite3_mprintf
    ("insert into log "
     "  (timestamp, tz, level, file, function, line, return_address, message)"
     " values (%"PRId64", %d, %d, %Q, %Q, %d, '0x%"PRIxPTR"', %Q);",
     n, tz, level, file, function, line, return_address, msg);

  /* Only initialize the buffer once we see an async message.  */
  if (! debug_output_buffer && async)
    debug_output_buffer = sqlq_new (debug_output_file, 8 * 4096, 30,
				    &sqlq_error_handler);

  /* If we have a debug buffer, we use it unconditionally to ensure
     messages are ordered chronologically.  If we don't have a debug
     buffer, then we haven't seen an async message yet (or we failed
     to allocate the buffer).  */
  if (debug_output_buffer)
    sqlq_append (debug_output_buffer, async ? false : true, sql);
  else
    {
      char *errmsg = NULL;
      sqlite3_exec (debug_output_file, sql, NULL, NULL, &errmsg);
      if (errmsg)
	{
	  fprintf (stderr, "%s\n", errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	}
    }

  sqlite3_free (sql);
#else
  DEBUG_STDERR(function, line, return_address, msg);
#endif

  free (msg);
  va_end (ap);
}

const char *
debug_init_ (void)
{
#ifdef LOG_TO_DB
  if (debug_output_file)
    return debug_output_filename;

  if (! debug_output_filename)
    debug_output_filename = files_logfile (DEBUG_OUTPUT_FILENAME);

  sqlite3 *db;
  int err = sqlite3_open (debug_output_filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   debug_output_filename, sqlite3_errmsg (db));

  debug_output_file = db;

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  char *errmsg = NULL;
  err = sqlite3_exec (db,
		      "create table if not exists log"
		      " (OID INTEGER PRIMARY KEY AUTOINCREMENT,"
		      /* TIMESTAMP is MS from the epoch in UTC.  TZ is
			 the local timezone's number of minutes from
			 UTC.  */
		      "  timestamp, tz,"
		      "  level, function, file, line, return_address,"
		      "  message);"
		      /* Keep about 100k records.  At 100 bytes each,
			 this is about 10MB.  */
		      "delete from log"
		      "  where ROWID < (select max(ROWID) from log) - 100000;",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      fprintf (stderr, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  /* Flush and destroy any buffer when the thread exists and close the
     database connection.  */
  void destructor (void *value)
  {
    if (debug_output_buffer)
      {
	sqlq_flush (debug_output_buffer);
	sqlq_free (debug_output_buffer);
	debug_output_buffer = NULL;
      }

    if (debug_output_file)
      {
	sqlite3_close (debug_output_file);
	debug_output_file = NULL;
      }
  }

  static pthread_key_t key;

  void set_destructor (void)
  {
    pthread_key_create (&key, destructor);
  }
  static pthread_once_t set_destructor_once = PTHREAD_ONCE_INIT;
  pthread_once (&set_destructor_once, set_destructor);

  pthread_setspecific (key, debug_output_file);

  return debug_output_filename;
#else
  return NULL;
#endif
}

void
blob_dump (const char *buffer, int bytes)
{
  int skip = 0;

  int chunk_size = sizeof (uint32_t) * 4;
  int chunk;
  int offset = 0;
  for (; bytes > 0; bytes -= chunk, buffer += chunk, offset += chunk)
    {
      chunk = bytes < chunk_size ? bytes : chunk_size;

      bool all_zeros = true;
      int i;
      for (i = 0; i < chunk; i ++)
	if (buffer[i])
	  {
	    all_zeros = false;
	    break;
	  }

      if (all_zeros)
	{
	  skip += chunk;
	  continue;
	}

      if (skip)
	{
	  printf ("  Skipped %d zero bytes.\n", skip);
	  skip = 0;
	}

      /* Print as hex.  */
      printf ("  %d:", offset);
      for (i = 0; i < chunk; i ++)
	{
	  if (i % sizeof (uint32_t) == 0)
	    printf (" ");
	  printf ("%02x", (unsigned char) buffer[i]);
	}

      printf ("  ");

      /* Print printable characters.  */
      for (i = 0; i < chunk; i ++)
	{
	  if (i % sizeof (uint32_t) == 0)
	    printf (" ");
	  putchar (isprint (buffer[i]) ? buffer[i] : '.');
	}
      putchar ('\n');
    }
  if (skip)
    printf ("  Skipped %d zero bytes.\n", skip);
}
