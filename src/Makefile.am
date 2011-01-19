bin_PROGRAMS = netczar

AM_CFLAGS = -Wall -g -O2 -std=gnu99 $(BASE_CFLAGS)
AM_CPPFLAGS = -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64

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

netczar_SOURCES = netczar.c $(nm_interfaces_h) marshal.h marshal.c \
	network-monitor.h network-monitor.c \
	debug.h debug.c \
	ll-networking-linux.h ll-networking-linux.c \
	org.walfield.NetCzar-server.h
netczar_LDADD = $(BASE_LIBS)

.PHONEY: xml-to-h
xml-to-h: $(patsubst %.xml,%.h,$(wildcard $(srcdir)/*.xml))

%.h: %.xml
	dbus-binding-tool --mode=glib-client "$<" > "$@"

%-server.h: %.xml
	echo "$*"
	dbus-binding-tool --mode=glib-server \
	  --prefix=$$(echo "$*" | sed 's/.*[/]//; s/[.]/_/g') \
	  "$<" > "$@"
