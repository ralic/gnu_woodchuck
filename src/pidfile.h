/* pidfile.h - Pidfile manager using sqlite3 backend interface.
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

#ifndef PIDFILE_H
#define PIDFILE_H

#include <unistd.h>

/* Returns the pid of the process that owns the lock file.  */
extern pid_t pidfile_check (const char *filename, const char *exe);

/* Removes the lock file.  */
extern void pidfile_remove (const char *filename);

/* Returns 0 if the lock file was successfully acquired, the pid of
   the process that likely owns the pid file, otherwise.  */
extern pid_t pidfile_acquire (const char *filename, const char *exe);

#endif
