AC_PREREQ(2.57)
AC_INIT(smart-storage, 0.0, neal@cs.jhu.edu)
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE()
AC_CONFIG_SRCDIR([smart-storage-logger.c])

AC_GNU_SOURCE

# Checks for programs.
AC_PROG_CC
AM_PROG_CC_C_O

AM_PROG_AS
AC_PROG_RANLIB

AC_CHECK_LIB([sqlite3], [sqlite3_close], [],
	     [AC_ERROR([libsqlite3 not found.  Please install sqlite3.])])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
