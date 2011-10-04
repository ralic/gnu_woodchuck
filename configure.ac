AC_PREREQ([2.59])
AC_INIT(woodchuck, 0.3, neal@walfield.org)
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE()
AC_CONFIG_SRCDIR([src/murmeltier.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC
AM_PROG_CC_C_O
AC_GNU_SOURCE

AM_PROG_AS
AC_PROG_RANLIB
AC_PROG_LIBTOOL

AM_PATH_PYTHON([2.4])

# If sphinx-build is not found, set it to :, in which case the
# documentation won't be rebuilt.
AC_PATH_PROG([SPHINX_BUILD], [sphinx-build], :)

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
	],
	[
                # If maemo-version is not installed, but icd2 is,
                # we're on Harmattan, which we call maemo version 6.
                if pkg-config --modversion icd2 >/dev/null
                then
		    AC_MSG_RESULT([yes, found Harmattan])
                    HAVE_MAEMO_VERSION=600
                else
		    AC_MSG_RESULT([no, configuring for a GTK-based environment])
                fi
	])
if test x$HAVE_MAEMO_VERSION != x
then
  AC_DEFINE_UNQUOTED([HAVE_MAEMO_VERSION], $HAVE_MAEMO_VERSION,
	[Version of Maemo (format: XXYY, XX: Major, Y: Minor).])
  AC_DEFINE_UNQUOTED(HAVE_MAEMO, 1, [Whether we are building for Maemo.])
fi
AM_CONDITIONAL([MAEMO], [test 0$HAVE_MAEMO_VERSION -gt 0])

DBUS_MODULES="dbus-1 dbus-glib-1"
PKG_CHECK_MODULES(DBUS, $DBUS_MODULES)
AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)

GLIB_MODULES="glib-2.0 gobject-2.0"
PKG_CHECK_MODULES(GLIB, $GLIB_MODULES)
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

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


AS_AC_EXPAND(DATADIR, $datadir)
DBUS_SERVICES_DIR="$DATADIR/dbus-1/services"
AC_SUBST(DBUS_SERVICES_DIR)
AC_DEFINE_UNQUOTED(DBUS_SERVICES_DIR,
	"$DBUS_SERVICES_DIR",
	[Where services dir for DBUS is])

AS_AC_EXPAND(SYSCONFDIR, $sysconfdir)
XSESSION_D_DIR="$SYSCONFDIR/X11/Xsession.d"
AC_SUBST(XSESSION_D_DIR)
AC_DEFINE_UNQUOTED(XSESSION_D_DIR,
	"$XSESSION_D_DIR",
	[X11's xsession.d directory.])

SUDOERS_D_DIR="$SYSCONFDIR/sudoers.d"
AC_SUBST(SUDOERS_D_DIR)
AC_DEFINE_UNQUOTED(SUDOERS_D_DIR,
	"$SUDOERS_D_DIR",
	[sudo's sudoers.d directory.])

AC_CONFIG_FILES([Makefile
		src/Makefile
		clients/Makefile
		include/Makefile
		data/Makefile
		doc/Makefile])
AC_OUTPUT
