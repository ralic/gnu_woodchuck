/* Work around a bug in <icd/dbus_api.h>, which has an artificial
   dependency on glib.  */
#if HAVE_MAEMO
#include_next <glib.h>
#endif
