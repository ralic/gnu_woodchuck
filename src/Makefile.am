bin_PROGRAMS = netczar logger process-tracer

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
	com.nokia.mce.request.h \
	com.nokia.mce.signal.h \
	org.freedesktop.Hal.Manager.xml \
	org.freedesktop.Hal.Device.xml \
	org.freedesktop.DBus.xml \
	org.walfield.NetCzar.xml \
	org.walfield.NetCzar.client.xml \
	, $(srcdir)/$(filename))

nm_interfaces_h = $(patsubst %.xml,%.h,$(nm_interfaces_xml))

nm_server_stubs_xml = $(foreach filename, \
	org.walfield.NetCzar.xml \
	org.walfield.NetCzar.client.xml \
	, $(srcdir)/$(filename))

nm_server_stubs_c = $(patsubst %.xml,%-server.h,$(nm_server_stubs_xml))

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
	process-monitor-ptrace.h process-monitor-ptrace.c

netczar_SOURCES = $(common_sources) netczar.c \
	org.walfield.NetCzar-server.h
netczar_LDADD = $(BASE_LIBS)

logger_SOURCES = $(common_sources) logger.c logger-uploader.h logger-uploader.c
logger_LDADD = $(BASE_LIBS)

process_tracer_CPPFLAGS = $(AM_CPPFLAGS) -DPROCESS_TRACER_STANDALONE
process_tracer_LDADD = $(BASE_LIBS)
process_tracer_SOURCES = process-monitor-ptrace.c process-monitor-ptrace.h \
	signal-handler.h signal-handler.c \
	debug.h debug.c

.PHONEY: xml-to-h
xml-to-h: $(patsubst %.xml,%.h,$(wildcard $(srcdir)/*.xml))

%.h: %.xml
	dbus-binding-tool --mode=glib-client "$<" > "$@"

%-server.h: %.xml
	echo "$*"
	dbus-binding-tool --mode=glib-server \
	  --prefix=$$(echo "$*" | sed 's/.*[/]//; s/[.]/_/g') \
	  "$<" > "$@"
