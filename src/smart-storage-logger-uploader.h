/* logger-uploader.h - Uploader.
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
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#ifndef LOGGER_UPLOADER_H
#define LOGGER_UPLOADER_H

/* Register a table that should be synchronized.  */
extern void logger_uploader_table_register
  (const char *filename, const char *table, bool delete_synchronized);

/* Initialize the uploader.  */
extern void logger_uploader_init (void);

#endif
