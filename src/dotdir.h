/* dotdir.c - Dot directory management.
   Copyright (C) 2011 Neal H. Walfield <neal@walfield.org>

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

#ifndef DOTDIR_H
#define DOTDIR_H

/* Initialize the dot directory.  APPLICATION is the application's
   identifier and should contain only apha-numeric characters.  The
   user's homedirectory plus a dot will be prepended to this to create
   the dot directory, e.g., if APPLICATION is foo, then the dot
   directory will be initialized to be "/home/user/.foo".  This also
   creates the directory if it does not exist.  Returns 0 on success,
   an errno error code on failure.  */
extern int dotdir_init (const char *application);

/* Return the absolute filename for a file named FILENAME in the
   subdirectory SUBDIR of the dot directory.  FILENAME should not
   include any directory components.  */
extern char *dotdir_filename (const char *subdir, const char *filename);

#endif
