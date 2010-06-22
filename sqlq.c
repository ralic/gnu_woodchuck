/* sqlq.c - SQL command queuer.
   Copyright (C) 2010 Neal H. Walfield <neal@walfield.org>

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

#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#include "sqlq.h"
#include "debug.h"

struct sqlq *
sqlq_new_static (sqlite3 *db, void *buffer, int size)
{
  struct sqlq *q = buffer;
  q->malloced = false;
  q->db = db;
  q->used = 0;
  q->size = size - sizeof (*q);

  return q;
}

struct sqlq *
sqlq_new (sqlite3 *db, int size)
{
  size += sizeof (struct sqlq);

  void *buffer = malloc (size);
  struct sqlq *q = sqlq_new_static (db, buffer, size);
  q->malloced = true;
  return q;
}

void
sqlq_free (struct sqlq *q)
{
  if (q->used != 0)
    debug (0, "sqlq_free with unflushed data!");

  /* Clear it (for debugging purposes).  */
  memset (q, 0, sizeof (*q) + q->size);

  if (q->malloced)
    free (q);
}

static void
flush (sqlite3 *db, const char *command_string1, const char *command_string2)
{
  const char *c[2] = { command_string1, command_string2 };

  debug (5, "Flushing (`%s', `%s')", c[0], c[1]);

  int i;
  for (i = 0; i < sizeof (c) / sizeof (c[0]); i ++)
    if (c[i] && c[i][0] != 0)
      break;
  if (i == sizeof (c) / sizeof (c[0]))
    /* All strings are empty.  Nothing to do.  */
    return;

  /* Wrap the command in a transaction.  */
  char *errmsg = NULL;
  sqlite3_exec (db, "begin transaction", NULL, NULL, &errmsg);
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

      sqlite3_exec (db, c[i], NULL, NULL, &errmsg);
      if (errmsg)
	{
	  debug (0, "%s -> %s", c[i], errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	}
    }
	
  sqlite3_exec (db, "end transaction", NULL, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "end transaction: %s", errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
    }
}

bool
sqlq_append (struct sqlq *q, bool force_flush, const char *command)
{
  /* Whether there is space for a string of length LEN (LEN does
     not include the NUL terminator).  */
  bool have_space (int len)
  {
    return len + 1 <= q->size - q->used;
  }

  int len = command ? strlen (command) : 0;
  if (force_flush || ! have_space (len))
    /* Need to flush the buffer first.  */
    {
      /* NUL terminate the string.  */
      q->buffer[q->used] = 0;
      q->used = 0;

      /* Flush the commands.  */
      flush (q->db, q->buffer, command);
    }
  else
    {
      memcpy (&q->buffer[q->used], command, len);
      q->used += len;
    }

  return q->used != 0;
}

bool
sqlq_append_printf (struct sqlq *sqlq, bool force_flush,
		    const char *command, ...)
{
  va_list ap;
  va_start (ap, command);

  char *s = sqlite3_vmprintf (command, ap);
  debug (5, "%s", s);

  bool ret = sqlq_append (sqlq, force_flush, s);

  sqlite3_free (s);

  va_end (ap);

  return ret;
}

void
sqlq_flush (struct sqlq *q)
{
  sqlq_append (q, true, NULL);
}

