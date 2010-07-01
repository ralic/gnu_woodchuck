#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <error.h>
#include <stdarg.h>

#include "debug.h"
#include "util.h"
#include "files.h"

#ifdef LOG_TO_DB
#include <sqlite3.h>
static sqlite3 *debug_output_file;
#endif

#if !defined(DEBUG_ELIDE)
int output_debug = 3;
#endif

void
debug_ (const char *file, const char *function, int line,
	void *return_address, int level, const char *fmt, ...)
{
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

  char *errmsg = NULL;
  sqlite3_exec_printf
    (debug_output_file,
     "insert into log "
     "  (timestamp, tz, level, file, function, line, return_address, message)"
     " values (%"PRId64", %d, %d, %Q, %Q, %d, '0x%"PRIxPTR"', %Q);",
     NULL, NULL, &errmsg,
     n, tz, level, file, function, line, return_address, msg);

  if (errmsg)
    {
      fprintf (stderr, "%s\n", errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }
#else
  time_t __t = time (NULL);
  struct tm __tm;
  localtime_r (&__t, &__tm);

  fprintf (stderr, "%d.%d.%d %d:%02d.%02d:%s:%d:(%p): %s\n",
	   1900 + __tm.tm_year, __tm.tm_mon + 1, __tm.tm_mday,
	   __tm.tm_hour, __tm.tm_min, __tm.tm_sec,
	   function, line, return_address, msg);
  fflush (stderr);
#endif

  free (msg);
  va_end (ap);
}

void
debug_init_ (void)
{
#ifdef LOG_TO_DB
  if (debug_output_file)
    return;

  char *filename = log_file ("log.db");

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

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
		      "  message);",
		      NULL, NULL, &errmsg);
  if (errmsg)
    {
      fprintf (stderr, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  uploader_table_register (filename, "log", true);
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
