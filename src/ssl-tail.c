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

  bool first = true;
  char *filter = NULL;
  bool follow = false;
  last = strdup ("(select max (ROWID) - 10 from log)");
  char *filename = files_logfile (DEBUG_OUTPUT_FILENAME);
  char *table = NULL;

  void usage (int status)
  {
    fprintf
      (stderr,
       "%s [--all] [--follow] [--file=LOG_FILE] [--table=TABLE] [FILTER]\n"
       "Dumps entries in %s.\n\n"
       "Filter is an SQL expression on level, timestamp (MS in UTC),\n"
       "function, file or line.\n"
       "\n"
       "To see all entries in the last hour, run:\n"
       "  %s --all 'timestamp / 1000 > strftime (\"%%s\", \"now\") - 60 * 60'\n"
       "\n"
       "To see all entries since the last start, run:\n"
       "  %s --all 'ROWID >= (select max (ROWID) from log where message like \"smart-storage-logger compiled on %%\")'\n",
       argv[0], filename, argv[0], argv[0]);
    exit (status);
  }

  int i;
  for (i = 1; i < argc; i ++)
    if (strcmp (argv[i], "--all") == 0)
      {
	free (last);
	last = strdup ("0");
      }
    else if (strcmp (argv[i], "-f") == 0
	     || strcmp (argv[i], "--follow") == 0)
      follow = true;
    else if (strcmp (argv[i], "--help") == 0
	     || strcmp (argv[i], "--usage") == 0)
      usage (0);
    else if (strncmp (argv[i], "--file=", 7) == 0)
      {
	free (filename);
	filename = strdup (&argv[i][7]);
	fprintf (stderr, "Using %s\n", filename);
      }
    else if (strncmp (argv[i], "--table=", 8) == 0)
      table = strdup (&argv[i][8]);
    else if (argv[i][0] == '-')
      {
	fprintf (stderr, "Unknown option: '%s'\n", argv[i]);
	usage (1);
      }
    else if (filter)
      /* Only one filter is supported.  */
      usage (1);
    else
      filter = argv[i];

  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   filename, sqlite3_errmsg (db));

  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  do
    {
      if (! first)
	/* We could use inotify, but this is far from performance
	   critical.  */
	sleep (1);

      char *sql = sqlite3_mprintf
	("select ROWID, timestamp, tz, function, file, line,"
	 " return_address, message from %s"
	 " where (ROWID > %s) %s %s %s"
	 " order by ROWID;",
	 table ?: "log",
	 last,
	 filter ? "and (" : "",
	 filter ?: "",
	 filter ? ")" : "");

      char *errmsg = NULL;
      sqlite3_exec (db, sql, callback, NULL, &errmsg);
      if (errmsg)
	{
	  fprintf (stderr, "%s\nSQL: %s\n", errmsg, sql);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	  return 1;
	}
      sqlite3_free (sql);
      
      first = false;
    }
  while (follow);

  return 0;
}
