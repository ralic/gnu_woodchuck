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
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include "config.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <error.h>

#include "debug.h"
#include "files.h"

#define STRINGIFY(s) STRINGIFY_(s)
#define STRINGIFY_(s) #s

char *base;
int base_len;
char *dot_dir;
int dot_dir_len;

void
files_init (void)
{
  /* This function is called before debug_init, thus don't use debug!  */
  if (base)
    return;

#ifdef HAVE_MAEMO
  /* Monitor the user's home directory even when run as root.  */
  base = "/home/user";
#else
  base = getenv ("HOME");
#endif
  base_len = strlen (base);

  asprintf (&dot_dir, "%s/"STRINGIFY(DOT_DIR), base);
  dot_dir_len = strlen (dot_dir);
}

char *
files_logfile (const char *filename)
{
  files_init ();

  /* This will fail in the common case that the directory already
     exists.  */
  mkdir (dot_dir, 0750);

  char *dir = NULL;
  if (asprintf (&dir, "%s/logs", dot_dir) < 0)
    error (1, 0, "out of memory");
  mkdir (dir, 0750);

  char *abs_filename = NULL;
  if (asprintf (&abs_filename, "%s/%s", dir, filename) < 0)
    error (1, 0, "out of memory");
  free (dir);

  return abs_filename;
}

