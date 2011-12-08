/* sqlq.c - SQL command queuer.
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
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <glib.h>
#include <assert.h>
#include <inttypes.h>

#include "sqlq.h"
#undef sqlq_append
#undef sqlq_append_printf
#include "debug.h"

struct statement
{
  const char *file;
  const char *func;
  int line;
  char sql[];
};

/* Round ADDR up to a multiple of uintptr_t.  */
static uintptr_t
alignment_fixup (uintptr_t addr)
{
  uintptr_t alignment = __alignof__ (uintptr_t);
  uintptr_t result = (addr + alignment - 1) & ~(alignment - 1);
  assertx (result >= addr, "%"PRIxPTR" >= %"PRIxPTR, result, addr);
  return result;
}

static void
default_error_handler (const char *file, const char *func, int line,
		       const char *sql, const char *error_message)
{
  debug (0, "%s:%s:%d: Executing %s: %s",
	 file, func, line, sql, error_message);
}

#define ERROR_HANDLER(__eh_q, __eh_file, __eh_func, __eh_line,	 \
		      __eh_sql, __eh_error_message)		 \
  (((__eh_q)->error_handler ?: default_error_handler)		 \
   ((__eh_file), (__eh_func), (__eh_line),			 \
    (__eh_sql), (__eh_error_message)))

struct sqlq *
sqlq_new_static (sqlite3 *db, void *buffer, int size, int flush_delay,
		 sqlq_error_handler_t error_handler)
{
  assert (sizeof (struct sqlq) <= size);

  struct sqlq *q = buffer;
  q->malloced = false;
  q->db = db;
  q->used = 0;
  q->size = size - sizeof (*q);
  q->flush_delay = flush_delay;
  q->flush_source = 0;
  q->error_handler = error_handler;

  return q;
}

struct sqlq *
sqlq_new (sqlite3 *db, int size, int flush_delay,
	  sqlq_error_handler_t error_handler)
{
  size += sizeof (struct sqlq);

  void *buffer = malloc (size);
  struct sqlq *q = sqlq_new_static (db, buffer, size, flush_delay,
				    error_handler);
  q->malloced = true;
  return q;
}

void
sqlq_free (struct sqlq *q)
{
  if (q->used != 0)
    {
      q->buffer[q->used] = 0;
      ERROR_HANDLER (q, NULL, NULL, 0, "",
		     "sqlq_free called, but still have unflushed data!");
    }

  if (q->flush_source)
    {
      g_source_remove (q->flush_source);
      q->flush_source = 0;
    }

  /* Clear it (for debugging purposes).  */
  memset (q, 0, sizeof (*q) + q->size);

  if (q->malloced)
    free (q);
}

static void
flush (struct sqlq *q, struct statement *statement)
{
  struct statement *statement_block = (void *) q->buffer;
  int statement_block_len = q->used;

  if (statement_block_len == 0 && ! statement)
    /* Nothing to do.  */
    return;

  /* Wrap the command in a transaction.  */
  bool nested_transaction = false;
  char *errmsg = NULL;
  sqlite3_exec (q->db, "begin transaction", NULL, NULL, &errmsg);
  if (errmsg)
    {
      /* Would be nice to have a real error code...  */
      nested_transaction
	= strstr (errmsg, "cannot start a transaction within a transaction");

      char *msg = NULL;
      asprintf (&msg, "begin transaction;\n(%s: %s)",
		statement ? statement->sql : "",
		nested_transaction ? "" : "dropping");
      ERROR_HANDLER (q, NULL, NULL, 0, msg, errmsg);
      free (msg);

      sqlite3_free (errmsg);
      errmsg = NULL;

      if (! nested_transaction)
	return;
    }

  /* Execute the commands.  */
  void execute (struct sqlq *q, struct statement *s)
  {
    char *errmsg = NULL;
    sqlite3_exec (q->db, s->sql, NULL, NULL, &errmsg);
    if (errmsg)
      {
	ERROR_HANDLER (q, s->file, s->func, s->line, s->sql, errmsg);
	sqlite3_free (errmsg);
	errmsg = NULL;
      }
  }

  /* First iterate over the command block.  If we are in a nested
     transaction, we don't empty the buffer: we don't want to print
     the same messages multiple times.  */
  if (! nested_transaction && statement_block)
    {
      struct statement *s = statement_block;
      int remaining = statement_block_len;
      while (remaining > 0)
	{
	  int len = alignment_fixup (sizeof (*s) + strlen (s->sql) + 1);
	  remaining -= len;
	  assert (remaining >= 0);

	  execute (q, s);

	  s = (void *) (uintptr_t) s + len;
	}
      assert (remaining == 0);

      /* We could add something to the buffer while executing this
	 code, e.g., if the ERROR_HANDLER callback uses an output
	 routine that uses sqlq.  Deal with that gracefully, i.e.,
	 without dropping any message.  */
      if (q->used != statement_block_len)
	{
	  if (q->used < statement_block_len)
	    /* WTF?!?.  */
	    {
	      debug (0, "Assertion failure: "
		     "q->used (%d) >= statement_block_len (%d)",
		     q->used, statement_block_len);
	      /* Attempt to recover.  */
	      q->used = statement_block_len;
	    }
	  else
	    memmove (q->buffer, &q->buffer[statement_block_len],
		     q->used - statement_block_len);
	}
      q->used -= statement_block_len;
      assert (q->used >= 0);
    }

  /* Then execute any "hanging command."  */
  if (statement)
    execute (q, statement);

  if (! nested_transaction)
    {
      sqlite3_exec (q->db, "end transaction", NULL, NULL, &errmsg);
      if (errmsg)
	{
	  ERROR_HANDLER (q, NULL, NULL, 0, "end transaction", errmsg);
	  sqlite3_free (errmsg);
	  errmsg = NULL;
	}
    }
}

static gboolean
do_delayed_flush (gpointer user_data)
{
  struct sqlq *q = user_data;

  assert (q->flush_source);
  q->flush_source = 0;

  debug (5, "Delayed flush (have %d bytes)", q->used);

  sqlq_flush (q);

  return false;
}

bool
sqlq_append (const char *file, const char *func, int line,
	     struct sqlq *q, bool force_flush, const char *sql)
{
  if (q->flush_delay == 0)
    force_flush = true;

  /* Append the command.  */
  struct statement *s = NULL;
  if (sql)
    {
      int free_space = q->size - q->used;

      struct statement *s = NULL;
      int sql_len = strlen (sql) + 1;
      int s_len = alignment_fixup ((uintptr_t) sizeof (*s) + sql_len);

      if (s_len <= free_space)
	{
	  s = (struct statement *) &q->buffer[q->used];
	  q->used += s_len;
	  free_space -= s_len;

	  if (free_space < sizeof (*s) + 30)
	    /* There is unlikely to be enough space for another
	       command.  Flush now to avoid allocating on the stack
	       later.  */
	    force_flush = true;
	}
      else
	{
	  s = alloca (s_len);
	  force_flush = true;
	}

      s->file = file;
      s->func = func;
      s->line = line;
      memcpy (s->sql, sql, sql_len);
    }

  if (force_flush)
    /* Flush the pending commands: either the user explicitly
       requested it, or we are out of space in our buffer.  */
    {
      flush (q, s);

      if (q->flush_source)
	{
	  g_source_remove (q->flush_source);
	  q->flush_source = 0;
	}
    }
  else
    {
      assert (! s);

      if (! q->flush_source)
	/* Wait at most Q->FLUSH_DELAY seconds before flushing.  */
	q->flush_source = g_timeout_add_seconds (q->flush_delay,
						 do_delayed_flush, q);
    }

  return q->used != 0;
}

bool
sqlq_append_printf (const char *file, const char *func, int line,
		    struct sqlq *sqlq, bool force_flush,
		    const char *sql_fmt, ...)
{
  va_list ap;
  va_start (ap, sql_fmt);

  char *s = sqlite3_vmprintf (sql_fmt, ap);
  debug (5, "%s", s);

  bool ret = sqlq_append (file, func, line, sqlq, force_flush, s);

  sqlite3_free (s);

  va_end (ap);

  return ret;
}

void
sqlq_flush (struct sqlq *q)
{
  sqlq_append (NULL, NULL, 0, q, true, NULL);
}

void
sqlq_flush_delay_set (struct sqlq *q, int flush_delay)
{
  if (q->flush_delay == flush_delay)
    return;

  sqlq_flush (q);
  assert (! q->flush_source);

  q->flush_delay = flush_delay;
}
