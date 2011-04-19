AC_PREREQ([2.59])
AC_INIT(woodchuck, 0.2, neal@walfield.org)
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE()
AC_CONFIG_SRCDIR([src/netczar.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC
AM_PROG_CC_C_O
AC_GNU_SOURCE

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
BASE_MODULES="dbus-1 glib-2.0 gobject-2.0 gthread-2.0 dbus-glib-1 sqlite3"
if test 0$HAVE_MAEMO_VERSION -gt 0
then
  AC_DEFINE_UNQUOTED(HAVE_ICD2, 1,
                     [Whether to support Nokia's ICD2.])
  BASE_MODULES="$BASE_MODULES icd2"
else
  PKG_CHECK_EXISTS(NetworkManager,
    [AC_DEFINE_UNQUOTED(HAVE_NETWORK_MANAGER, 1,
                        [Whether to support NetworkManager.])])
  BASE_MODULES="$BASE_MODULES NetworkManager"
fi

PKG_CHECK_MODULES(BASE, $BASE_MODULES)
AC_SUBST(BASE_CFLAGS)
AC_SUBST(BASE_LIBS)

GLIB_GENMARSHAL=`$PKG_CONFIG glib-2.0 --variable=glib_genmarshal`
AC_SUBST(GLIB_GENMARSHAL)

AC_CONFIG_FILES([Makefile
		src/Makefile
		include/Makefile
		include/netczar/Makefile])
AC_OUTPUT
