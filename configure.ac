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

# Check whether we are running on Maemo and which version.
AC_MSG_CHECKING([for a Maemo environment])
PKG_CHECK_EXISTS(maemo-version,
	[
		HAVE_MAEMO_VERSION_RAW=$(pkg-config --modversion maemo-version)
		AC_MSG_RESULT([yes, version $HAVE_MAEMO_VERSION_RAW])
	        # Extract the version as four digit number, AABB where
	        # AA is the major version number and BB is the minor
	        # version number.
		HAVE_MAEMO_VERSION=$(echo "$HAVE_MAEMO_VERSION_RAW" \
		   | awk 'BEGIN { FS="." }
		          /[0-9]+([.][0-9]+)?([.][0-9]+)?/
                            { printf ("%d%02d", $1, $2); }
                         ')
                AC_DEFINE_UNQUOTED([HAVE_MAEMO_VERSION], $HAVE_MAEMO_VERSION,
                          [Version of Maemo (format: XXYY, XX: Major, Y: Minor).])
	],
	[
		AC_MSG_RESULT([no, configuring for a GTK-based environment])
	])
if test x$HAVE_MAEMO_VERSION != x
then
  AC_DEFINE_UNQUOTED(HAVE_MAEMO, 1, [Whether we are building for Maemo.])
fi
AM_CONDITIONAL([MAEMO], [test 0$HAVE_MAEMO_VERSION -gt 0])
AM_CONDITIONAL([MAEMO5], [test 0$HAVE_MAEMO_VERSION -ge 500])

# Modules that we require and can check for using pkg-config.
BASE_MODULES="dbus-1 sqlite3 icd2"
PKG_CHECK_MODULES(BASE, $BASE_MODULES)
AC_SUBST(BASE_CFLAGS)
AC_SUBST(BASE_LIBS)



AC_CONFIG_FILES([Makefile])
AC_OUTPUT
