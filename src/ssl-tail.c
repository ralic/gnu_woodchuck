#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <error.h>
#include <stdio.h>
#include <sqlite3.h>
#include <unistd.h>

#include "debug.h"
#include "files.h"
#include "util.h"

int
main (int argc, char *argv[])
{
  files_init ();

  char *filename = files_logfile (DEBUG_OUTPUT_FILENAME);
  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  char *last = NULL;
  int callback (void *cookie, int argc, char **argv, char **names)
  {
    int i = 0;
    const char *ROWID = argv[i ++];
    const char *timestamp = argv[i ++];
    const char *tz = argv[i ++];
    const char *function = argv[i ++];
    const char *file = argv[i ++];
    const char *line = argv[i ++];
    const char *return_address = argv[i ++];
    const char *msg = argv[i ++];

    free (last);
    last = strdup (ROWID);

    time_t t = (atoll (timestamp) / 1000) + atoi (tz) * 60;
    struct tm tm;
    gmtime_r (&t, &tm);

    printf ("%d.%d.%d %d:%02d.%02d:%s:%s:(%s): %s\n",
	    1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday,
	    tm.tm_hour, tm.tm_min, tm.tm_sec,
	    function, line, return_address, msg);

    return 0;
  }

  void usage (int status)
  {
    fprintf (stderr,
	     "%s [--all] [FILTER]\nFilter on level, timestamp (MS in UTC), function, file or line.\n",
	     argv[0]);
    exit (status);
  }

  bool first = true;
  char *filter = NULL;

  int i;
  for (i = 1; i < argc; i ++)
    if (strcmp (argv[i], "--all") == 0)
      {
	first = false;
	last = strdup ("0");
      }
    else if (filter)
      usage (1);
    else
      filter = argv[i];

  for (;;)
    {
      char *errmsg = NULL;
      sqlite3_exec_printf
	(db,
	 "select ROWID, timestamp, tz, function, file, line, "
	 "  return_address, message from log"
	 " where (%s%s) %s %s %s"
	 " order by ROWID;",
	 callback, NULL, &errmsg,
	 first
	   ? "timestamp > (select max (timestamp) - 10000 from log)"
	   : "ROWID > ",
	 first ? "" : last,
	 filter ? "and (" : "",
	 filter ?: "",
	 filter ? ")" : "");
      if (errmsg)
	{
	  fprintf (stderr, "%s\n", errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	  return 1;
	}
      first = false;

      /* We could use inotify, but this is far from performance
	 critical.  */
      sleep (1);
    }

  return 0;
}
