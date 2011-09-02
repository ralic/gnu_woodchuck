EXTRA_DIST =
BUILT_SOURCES =
CLEANFILES =

do_subst = sed -e 's,[@]bindir[@],$(bindir),g'

bin_PROGRAMS = murmeltier smart-storage-logger smart-storage-logger-recover \
	process-tracer ssl-tail gwoodchuck
bin_SCRIPTS = smart-storage-logger-consent
CLEANFILES += $(bin_SCRIPTS)

lib_LTLIBRARIES = libgwoodchuck-0.0.la
python_PYTHON = woodchuck.py pywoodchuck.py

# DBus service files
servicedir = $(DBUS_SERVICES_DIR)
service_in_files = org.woodchuck.service.in
service_DATA = $(service_in_files:.service.in=.service)

EXTRA_DIST += $(service_in_files)
CLEANFILES += $(service_DATA)

# Rule to make the service file with bindir expanded
$(service_DATA): $(service_in_files) Makefile
	$(do_subst) < "$<" > "$@"

# X11 Xsession.d files.
xsessiondir = $(XSESSION_D_DIR)
xsession_in_files = 98murmeltier-xsession.sh.in
xsession_DATA = $(xsession_in_files:.in=)

EXTRA_DIST += $(xsession_in_files)
CLEANFILES += $(xsession_DATA)

# Rule to make the service file with bindir expanded
$(xsession_DATA): $(xsession_in_files) Makefile
	$(do_subst) < "$<" > "$@"


# Turn a python program into a program.
%: %.py
	cp "$<" "$@"

AM_CFLAGS = -Wall -g -O2 -std=gnu99 $(BASE_CFLAGS)
AM_CPPFLAGS = -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
	-DPKGDATADIR=\"$(pkgdatadir)\" -I$(top_srcdir)/include

# DBus interfaces.
dbus_interfaces_xml = \
	org.freedesktop.DBus.Properties.xml \
	org.freedesktop.NetworkManager.Device.xml \
	org.freedesktop.NetworkManager.Device.Gsm.xml \
	org.freedesktop.NetworkManager.Device.Serial.xml \
	org.freedesktop.NetworkManager.Device.Wired.xml \
	org.freedesktop.NetworkManager.Device.Wireless.xml \
	org.freedesktop.NetworkManager.AccessPoint.xml \
	org.freedesktop.NetworkManager.xml \
	org.freedesktop.NetworkManager.Connection.Active.xml \
	com.nokia.icd2.xml \
	org.freedesktop.ConsoleKit.xml \
	com.nokia.mce.request.xml \
	com.nokia.mce.signal.xml \
	com.nokia.dsme.signal.xml \
	org.freedesktop.Hal.Manager.xml \
	org.freedesktop.Hal.Device.xml \
	org.freedesktop.DBus.xml \
	Phone.Net.xml \
	org.woodchuck.xml \
	org.woodchuck.manager.xml \
	org.woodchuck.stream.xml \
	org.woodchuck.object.xml \
	org.woodchuck.upcall.xml
EXTRA_DIST += $(dbus_interfaces_xml)

# Generate C client header files from the DBus interface specifications.
dbus_interfaces_h = $(patsubst %.xml,%.h,$(dbus_interfaces_xml))
BUILT_SOURCES += $(dbus_interfaces_h)

%.h: %.xml
	dbus-binding-tool --mode=glib-client "$<" > "$@"

# Generate C server stubs from the DBus interface specifications.
dbus_server_stubs_c = org.woodchuck.upcall.server-stubs.h
BUILT_SOURCES += $(dbus_server_stubs_c)

%.server-stubs.h: %.xml
	echo "$*"
	dbus-binding-tool --mode=glib-server \
	  --prefix=$$(echo "$*" | sed 's/.*[/]//; s/[.]/_/g') \
	  "$<" > "$@"

# Additional glib marhsallers.
EXTRA_DIST += marshal.list
BUILT_SOURCES += marshal.h marshal.c

%.h: %.list
	$(GLIB_GENMARSHAL) --header $< > $@~ \
	  && (if cmp -s $@ $@~; then rm $@~; else mv $@~ $@; fi)
%.c: %.list
	$(GLIB_GENMARSHAL) --body $< > $@~ \
	  && (if cmp -s $@ $@~; then rm $@~; else mv $@~ $@; fi)

# DBus specifications as C strings.  Used by murmeltier-dbus-server.c
# to implement org.freedesktop.DBus.Introspectable.introspect.
dbus_interfaces_xml_h = \
	org.woodchuck.xml.h \
	org.woodchuck.manager.xml.h \
	org.woodchuck.stream.xml.h \
	org.woodchuck.object.xml.h \
	org.freedesktop.DBus.Introspectable.xml.h \
	org.freedesktop.DBus.Properties.xml.h
BUILT_SOURCES += $(dbus_interfaces_xml_h)

# Extract each interface and save it as a CPP define named after the
# interface (uppercase and dots replaced by underscores) and _XML
# appeneded.  Be careful to escape \s and "s.
%.xml.h: %.xml
	gawk 'function escape(s) { \
	        return gensub (/([\\"])/, "\\\\\\1", "g", s); \
	      } \
	      BEGIN { print "/* Automatically generated from $<. */"; } \
	      { \
	        while (1) { \
	          if (! in_interface && match($$0, /<interface +name=["]([^"]+)["]>/, matches)) \
	            { \
	              gsub (/[.]/, "_", matches[1]); \
	              printf ("#define %s_XML ", toupper (matches[1])); \
	              \
	              if (match (substr($$0, RSTART), /[^ \t]/)) \
	                $$0=substr($$0, RSTART); \
	              in_interface=1; \
	            } \
	          if (in_interface && match ($$0, /<[/]interface>/)) \
	            { \
	              in_interface=0; \
	              print "\"" escape(substr($$0, 0, RSTART+RLENGTH-1)) "\\n\""; \
	              $$0=substr($$0, RSTART+RLENGTH); \
	              continue; \
	            } \
	          if (in_interface) \
	            print "\"" escape($$0) "\\n\" \\"; \
	          break; \
	        } \
	      }' "$<" > "$@"

# network-monitor-*.c are #included from network-monitor.c, as
# appropriate.
EXTRA_DIST += network-monitor-icd2.c network-monitor-nm.c

# Debugging machinery.
debug_src = debug.h debug.c
# If you define LOG_TO_DB, you also need the following files and
# you'll need to link to sqlite.
debug_log_to_db_src = $(debug_src) util.h files.h

# The monitors and their dependencies (except for debug; choose either
# debug_src or debug_log_to_db_src).
monitors = \
	network-monitor.h network-monitor.c \
	ll-networking-linux.h ll-networking-linux.c \
	dbus-util.h \
	\
	user-activity-monitor.h user-activity-monitor.c \
	\
	battery-monitor.h battery-monitor.c \
	\
	service-monitor.h service-monitor.c \
	\
	process-monitor-ptrace.h process-monitor-ptrace.c \
	signal-handler.h signal-handler.c \
	files.h files.c \
	\
	shutdown-monitor.h shutdown-monitor.c \
	\
	marshal.h marshal.c

murmeltier_SOURCES = $(dbus_interfaces_h) $(monitors) \
	$(dbus_interfaces_xml_h) \
	murmeltier.c \
	murmeltier-dbus-server.h murmeltier-dbus-server.c \
	org.woodchuck.xml.h \
	org.woodchuck.manager.xml.h \
	org.woodchuck.stream.xml.h \
	org.woodchuck.object.xml.h \
	org.freedesktop.DBus.Introspectable.xml.h \
	dotdir.h dotdir.c \
	$(debug_log_to_db_src) \
	util.h
murmeltier_CPPFLAGS = $(AM_CPPFLAGS) -DLOG_TO_DB -DDOT_DIR=.murmeltier
murmeltier_LDADD = $(BASE_LIBS)

smart_storage_logger_SOURCES = $(dbus_interfaces_h) $(monitors) \
	smart-storage-logger.c \
	smart-storage-logger-uploader.h smart-storage-logger-uploader.c \
	pidfile.h pidfile.c \
	md5.h md5.c \
	sqlq.h sqlq.c \
	util.h \
	$(debug_log_to_db_src)
smart_storage_logger_CPPFLAGS = $(AM_CPPFLAGS) \
	-DLOG_TO_DB -DDOT_DIR=.smart-storage
smart_storage_logger_LDADD = $(BASE_LIBS)

smart_storage_logger_recover_SOURCES = \
	smart-storage-logger-recover.c \
	files.h files.c \
	$(debug_log_to_db_src) \
	util.h
smart_storage_logger_recover_CPPFLAGS = $(AM_CPPFLAGS) \
	-DLOG_TO_DB -DDOT_DIR=.smart-storage
# Needs sqlite, glib and pthreads.
smart_storage_logger_recover_LDADD = $(BASE_LIBS)

process_tracer_SOURCES = process-monitor-ptrace.c process-monitor-ptrace.h \
	signal-handler.h signal-handler.c \
	util.h \
	$(debug_src)
process_tracer_CPPFLAGS = $(AM_CPPFLAGS) -DPROCESS_TRACER_STANDALONE
# Needs sqlite, glib and pthreads.
process_tracer_LDADD = $(BASE_LIBS)

ssl_tail_SOURCES = ssl-tail.c files.h files.c debug.h util.h
ssl_tail_CPPFLAGS = $(AM_CPPFLAGS) -DDOT_DIR=.smart-storage
# Needs sqlite.
ssl_tail_LDADD = $(BASE_LIBS)

libgwoodchuck_0_0_la_SOURCES = \
	gwoodchuck.c \
	$(dbus_interfaces_h) \
	org.woodchuck.upcall.server-stubs.h
libgwoodchuck_0_0_la_CPPFLAGS = $(AM_CPPFLAGS) $(DBUS_CFLAGS) $(GLIB_CFLAGS)

gwoodchuck_SOURCES = $(libgwoodchuck_0_0_la_SOURCES)
gwoodchuck_CPPFLAGS = $(libgwoodchuck_0_0_la_CPPFLAGS) -DGWOODCHUCK_TEST
gwoodchuck_LDADD = $(DBUS_LIBS) $(GLIB_LIBS)

EXTRA_DIST += smart-storage-logger-consent.py