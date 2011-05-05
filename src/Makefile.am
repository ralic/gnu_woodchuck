bin_PROGRAMS = murmeltier smart-storage-logger smart-storage-logger-recover \
	process-tracer ssl-tail
bin_SCRIPTS = smart-storage-logger-consent

%: %.py
	cp "$<" "$@"

AM_CFLAGS = -Wall -g -O2 -std=gnu99 $(BASE_CFLAGS)
AM_CPPFLAGS = -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -DPKGDATADIR=\"$(pkgdatadir)\"
nm_interfaces_xml = $(foreach filename, \
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
	org.woodchuck.upcall.xml \
	, $(srcdir)/$(filename))

nm_interfaces_h = $(patsubst %.xml,%.h,$(nm_interfaces_xml))

nm_server_stubs_xml = $(foreach filename, \
	org.woodchuck.xml \
	org.woodchuck.upcall.xml \
	, $(srcdir)/$(filename))

nm_server_stubs_c = $(patsubst %.xml,%.server-stubs.h,$(nm_server_stubs_xml))

BUILT_SOURCES = $(nm_interfaces_h) $(nm_server_stubs_c) \
	marshal.h marshal.c

# network-monitor-*.c are #included from network-monitor.c, as
# appropriate.
EXTRA_DIST = marshal.list network-monitor-icd2.c network-monitor-nm.c

%.h: %.list
	$(GLIB_GENMARSHAL) --header $< > $@~ \
	  && (if cmp -s $@ $@~; then rm $@~; else mv $@~ $@; fi)
%.c: %.list
	$(GLIB_GENMARSHAL) --body $< > $@~ \
	  && (if cmp -s $@ $@~; then rm $@~; else mv $@~ $@; fi)

common_sources = $(nm_interfaces_h) marshal.h marshal.c \
	debug.h debug.c \
	files.h files.c \
	sqlq.h sqlq.c \
	signal-handler.h signal-handler.c \
	network-monitor.h network-monitor.c \
	ll-networking-linux.h ll-networking-linux.c \
	user-activity-monitor.h user-activity-monitor.c \
	battery-monitor.h battery-monitor.c \
	service-monitor.h service-monitor.c \
	process-monitor-ptrace.h process-monitor-ptrace.c \
	shutdown-monitor.h shutdown-monitor.c

murmeltier_SOURCES = $(common_sources) murmeltier.c \
	org.woodchuck.murmeltier-server.h
murmeltier_LDADD = $(BASE_LIBS)

smart_storage_logger_SOURCES = $(common_sources) \
	smart-storage-logger.c \
	smart-storage-logger-uploader.h smart-storage-logger-uploader.c \
	pidfile.h pidfile.c \
	md5.h md5.c
smart_storage_logger_CPPFLAGS = $(AM_CPPFLAGS) -DLOG_TO_DB
smart_storage_logger_LDADD = $(BASE_LIBS)

smart_storage_logger_recover_SOURCES = \
	smart-storage-logger-recover.c \
	debug.h debug.c \
	files.h files.c
smart_storage_logger_recover_CPPFLAGS = $(AM_CPPFLAGS) -DLOG_TO_DB
smart_storage_logger_recover_LDADD = $(BASE_LIBS)

process_tracer_CPPFLAGS = $(AM_CPPFLAGS) -DPROCESS_TRACER_STANDALONE
process_tracer_LDADD = $(BASE_LIBS)
process_tracer_SOURCES = process-monitor-ptrace.c process-monitor-ptrace.h \
	signal-handler.h signal-handler.c \
	debug.h debug.c

ssl_tail_SOURCES = ssl-tail.c files.h files.c debug.h util.h
ssl_tail_CPPFLAGS = $(AM_CPPFLAGS)
ssl_tail_LDADD = $(BASE_LIBS)

.PHONEY: xml-to-h
xml-to-h: $(patsubst %.xml,%.h,$(wildcard $(srcdir)/*.xml))

%.h: %.xml
	dbus-binding-tool --mode=glib-client "$<" > "$@"

%.server-stubs.h: %.xml
	echo "$*"
	dbus-binding-tool --mode=glib-server \
	  --prefix=$$(echo "$*" | sed 's/.*[/]//; s/[.]/_/g') \
	  "$<" > "$@"
