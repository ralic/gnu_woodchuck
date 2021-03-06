/* files.c - File management.
   Copyright (C) 2009, 2010 Neal H. Walfield <neal@walfield.org>

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

#ifndef FILES_H
#define FILES_H

#include <string.h>
#include <stdbool.h>
#include <assert.h>

extern void files_init (void);

/* The directory under which we monitor files for changes (not
   including a trailing slash).  */
extern char *base;
/* strlen (base).  */
extern int base_len;

/* The directory (under the user's home directory) in which we store
   the log file.  */
#ifndef DOT_DIR
# define DOT_DIR .woodchuck
#endif
/* The directory's absolute path.  */
extern char *dot_dir;
/* strlen (dot_dir).  */
extern int dot_dir_len;


/* Returns whether FILENAME is DOT_DIR or is under DOT_DIR.  */
static inline bool
under_dot_dir (const char *filename)
{
  assert (dot_dir);

  return (strncmp (filename, dot_dir, dot_dir_len) == 0
	  && (filename[dot_dir_len] == '\0'
	      || filename[dot_dir_len] == '/'));
}

/* Given a base (e.g., foo.db), return an absolute path (e.g.,
   /home/user/.smart-storage/logs/foo.db).  Caller must free the
   returned string.  */
extern char *files_logfile (const char *base);

#endif
