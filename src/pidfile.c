/* pidfile.c - Pidfile manager using sqlite3 backend.
   Copyright (C) 2010 Neal H. Walfield <neal@walfield.org>

   Woodchuck is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 3, or (at
   your option) any later version.

   Woodchuck is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <unistd.h>
#include <errno.h>

#include "util.h"
#include "debug.h"

/* Open a pid file.  */
static sqlite3 *
pidfile_open (const char *filename)
{
  sqlite3 *db;
  int err = sqlite3_open (filename, &db);
  if (err)
    {
      debug (0, "sqlite3_open (%s): %s",
	     filename, sqlite3_errmsg (db));
      abort ();
    }

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  /* Figure out if the table has already been created.  */
  int count = 0;
  int callback (void *cookie, int argc, char **argv, char **names)
  {
    count = atoi (argv[0]);
    return 0;
  }

  char *errmsg = NULL;
  err = sqlite3_exec (db,
		      "select count (*) from sqlite_master"
		      " where type='table' and name='pid';",
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
      err = sqlite3_exec (db,
			  "create table pid (pid, exe);",
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
      debug (0, "%d tables with name `pid'?!?", count);
      abort ();
    }

  return db;
}

struct data
{
  pid_t pid;
  char *exe;
};

static void
free_data (struct data *data)
{
  free (data->exe);
}

/* Read the data from a PID file.  */
static struct data
pidfile_read (const char *filename, sqlite3 *db)
{
  struct data data;
  memset (&data, 0, sizeof (data));

  int count = 0;
  int callback (void *cookie, int argc, char **argv, char **names)
  {
    data.pid = atoi (argv[0]);
    data.exe = strdup (argv[1]);
    count ++;

    if (count > 1)
      {
	if (count == 2)
	  {
	    debug (0, "Multiple pid records found in %s", filename);
	    debug (0, "#1: %d %s", data.pid, data.exe);
	  }
	debug (0, "#%d: %s %s", count, argv[0], argv[1]);
      }

    return 0;
  }

  char *errmsg = NULL;
  sqlite3_exec_printf (db,
		       "select pid, exe from pid;",
		       callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%s", errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
      abort ();
    }

  if (count > 1)
    /* Assume the file is corrupted.  */
    {
      debug (0, "Assuming pidfile (%s) is corrupted.  "
	     "Continuing and hoping for the best.",
	     filename);
      memset (&data, 0, sizeof (data));
    }
  else if (count == 1)
    debug (0, "Owner: %d (%s)", data.pid, data.exe);

  return data;
}

static pid_t
pidfile_check_ll (const char *filename, sqlite3 *db, const char *expected_exe)
{
  struct data data = pidfile_read (filename, db);
  if (data.pid == 0)
    /* No pid file.  */
    {
      free_data (&data);
      return 0;
    }

  /* The pid file seems to exist.  See if it is stale.  */
  char exe_link[100];
  snprintf (exe_link, sizeof (exe_link), "/proc/%d/exe", data.pid);
  char exe[4096];
  int len = readlink (exe_link, exe, sizeof (exe) - 1);
  if (len == -1)
    {
      if (errno == EACCES)
	/* Can't read the symlink.  Assume that the program running
	   under this PID is really an instance of EXPECTED_EXE.  */
	{
	  debug (0, "readlink(%s): %m", exe_link);
	  return data.pid;
	}

      if (errno == ENOENT)
	/* File does not exist (and thus, neither does a process with
	   pid PID).  */
	{
	  debug (0, "%s does not exist.  Ignoring stale pid file (%s).",
		 exe_link, filename);
	  return 0;
	}

      /* For other errors, assume it is ok.  */
      debug (0, "readlink(%s): %m", exe_link);
      return 0;
    }
  exe[len] = 0;

  /* See if PID's executable matches EXPECTED_EXE.  */
  const char *exe_base = strrchr (exe, '/');
  if (! exe_base)
    exe_base = exe;
  else
    exe_base = exe_base + 1;

  if (strcmp (expected_exe, exe_base) == 0)
    {
      debug (0, "%s running (pid: %d)", expected_exe, data.pid);
      return data.pid;
    }
  else
    {
      debug (0, "Stale pid file (%s).  Owned by pid %d, which is %s",
	     filename, data.pid, data.exe);
      return 0;
    }
}

pid_t
pidfile_check (const char *filename, const char *expected_exe)
{
  sqlite3 *db = pidfile_open (filename);
  if (! db)
    return 0;

  pid_t pid = pidfile_check_ll (filename, db, expected_exe);

  sqlite3_close (db);

  return pid;
}

static void
pidfile_remove_ll (const char *filename, sqlite3 *db)
{
  char *errmsg = NULL;
  sqlite3_exec_printf (db,
		       "delete from pid where pid = %d;",
		       NULL, NULL, &errmsg, getpid ());
  if (errmsg)
    {
      debug (0, "%s: %s", filename, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }
}

/* Abandon a pid file.  */
void
pidfile_remove (const char *filename)
{
  sqlite3 *db = pidfile_open (filename);

  pidfile_remove_ll (filename, db);

  sqlite3_close (db);
}

/* To acquire a pid file, we:

     - begin a transaction
       - check if there is a process running
       - if not, assume ownership
     - end transaction

   This is atomic and racefree.
*/
pid_t
pidfile_acquire (const char *filename, const char *exe)
{
  sqlite3 *db = pidfile_open (filename);

  char *errmsg = NULL;
  sqlite3_exec (db, "begin transaction;", NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%s: %s", filename, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }

  pid_t pid = pidfile_check_ll (filename, db, exe);
  if (pid == 0)
    /* We're good.  Assume ownership.  */
    {
      sqlite3_exec_printf (db,
			   /* Clear the table.  */
			   "delete from pid;"
			   /* Insert ourself.  */
			   "insert into pid values (%d, %Q);"
			   "commit transaction;",
			   NULL, NULL, &errmsg, getpid (), exe);
      if (errmsg)
	{
	  debug (0, "%s: %s", filename, errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	}
    }

  /* If we haven't committed the transaction, closing the DB will
     automatically roll back any pending transaction.  */
  sqlite3_close (db);

  return pid;
}
