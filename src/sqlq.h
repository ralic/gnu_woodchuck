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

#ifndef SQLQ_H
#define SQLQ_H

#include <sqlite3.h>
#include <stdbool.h>

struct sqlq
{
  sqlite3 *db;
  int used;
  int size;
  bool malloced;
  char buffer[0];
};

/* Allocate a new SQL command queue with a size of SIZE.  */
extern struct sqlq *sqlq_new (sqlite3 *db, int size);

/* Allocate a new SQL command queue from the buffer BUFFER, which has
   a size of SIZE.  */
extern struct sqlq *sqlq_new_static (sqlite3 *db, void *buffer, int size);

/* Release a command queue.  Does not flush any pending commands!  */
extern void sqlq_free (struct sqlq *sqlq);

/* Append a command to the queue.  May flush the queue if there is not
   enough space.  If FORCE_FLUSH is true, always flushes the queue.
   Returns true if there is buffered data, false otherwise.  */
extern bool sqlq_append (struct sqlq *sqlq, bool force_flush,
			 const char *command);

/* Append a command to the queue.  May flush the queue if there is not
   enough space.  If FORCE_FLUSH is true, always flushes the
   queue.  */
extern bool sqlq_append_printf (struct sqlq *sqlq, bool force_flush,
				const char *command, ...);

/* Flushes the sql command queue.  */
extern void sqlq_flush (struct sqlq *sqlq);

#endif
