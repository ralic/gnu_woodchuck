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
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.  */

#ifndef SQLQ_H
#define SQLQ_H

#include <sqlite3.h>
#include <stdbool.h>

typedef void (*sqlq_error_handler_t) (const char *file, const char *func,
				      int line, const char *sql,
				      const char *error_message);

struct sqlq
{
  sqlite3 *db;
  int used;
  int size;
  bool malloced;
  int flush_delay;
  int flush_source;
  sqlq_error_handler_t error_handler;
  char buffer[0];
};

/* Allocate a new SQL command queue with a size of SIZE.  FLUSH_DELAY
   is the maximum number of seconds we can delay flushing any command.
   ERROR_HANDLER is a function that will be called if an error occurs
   while executing some SQL.  It may be NULL, in which case the
   debugging module is used.  */
extern struct sqlq *sqlq_new (sqlite3 *db, int size, int flush_delay,
			      sqlq_error_handler_t error_handler);

/* Allocate a new SQL command queue from the buffer BUFFER, which has
   a size of SIZE.  */
extern struct sqlq *sqlq_new_static (sqlite3 *db, void *buffer, int size,
				     int flush_delay,
				     sqlq_error_handler_t error_handler);

/* Release a command queue.  Does not flush any pending commands!  */
extern void sqlq_free (struct sqlq *sqlq);

/* Append a command to the queue.  May flush the queue if there is not
   enough space.  If FORCE_FLUSH is true, always flushes the queue.
   Returns true if there is buffered data, false otherwise.

   NOTE: You do not need to specify the FILE, FUNC AND LINE arguments,
   this will be filled in automatically via macro expansion to the
   correspond to the call site.  */
extern bool sqlq_append (const char *file, const char *func, int line,
			 struct sqlq *sqlq, bool force_flush,
			 const char *command);

#define sqlq_append(_sa_sqlq, _sa_force_flush, _sa_command) \
  sqlq_append(__FILE__, __func__, __LINE__,		    \
	      _sa_sqlq, _sa_force_flush, _sa_command)

/* Append a command to the queue.  May flush the queue if there is not
   enough space.  If FORCE_FLUSH is true, always flushes the queue.

   NOTE: You do not need to specify the FILE, FUNC AND LINE arguments,
   this will be filled in automatically via macro expansion to the
   correspond to the call site.  */
extern bool sqlq_append_printf (const char *file, const char *func, int line,
				struct sqlq *sqlq, bool force_flush,
				const char *command, ...);

#define sqlq_append_printf(_sa_sqlq, _sa_force_flush, _sa_command, ...)	\
  sqlq_append_printf(__FILE__, __func__, __LINE__,			\
		     _sa_sqlq, _sa_force_flush, _sa_command, ##__VA_ARGS__)

/* Flushes the sql command queue.  */
extern void sqlq_flush (struct sqlq *sqlq);

/* Set the queue's flush delay to FLUSH_DELAY.  */
extern void sqlq_flush_delay_set (struct sqlq *q, int flush_delay);

#endif
