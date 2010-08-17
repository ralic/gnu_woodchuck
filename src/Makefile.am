bin_PROGRAMS = netczar

AM_CFLAGS = -Wall -g -O2 -std=gnu99 $(BASE_CFLAGS)
AM_CPPFLAGS = -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64

nm_interfaces = $(foreach filename, \
	org.freedesktop.DBus.Properties.xml \
	org.freedesktop.NetworkManager.Device.xml \
	org.freedesktop.NetworkManager.Device.Gsm.xml \
	org.freedesktop.NetworkManager.Device.Serial.xml \
	org.freedesktop.NetworkManager.Device.Wired.xml \
	org.freedesktop.NetworkManager.Device.Wireless.xml \
	org.freedesktop.NetworkManager.AccessPoint.xml \
	org.freedesktop.NetworkManager.xml \
	org.freedesktop.NetworkManager.Connection.Active.xml \
	com.nokia.icd2.xml, \
	$(srcdir)/$(filename))

nm_interfaces_h = $(patsubst %.xml,%.h,$(nm_interfaces))

BUILT_SOURCES = $(nm_interfaces_h) marshal.h marshal.c

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
	ll-networking-linux.h ll-networking-linux.c
netczar_LDADD = $(BASE_LIBS)

.PHONEY: xml-to-h
xml-to-h: $(patsubst %.xml,%.h,$(wildcard $(srcdir)/*.xml))

%.h: %.xml
	dbus-binding-tool --mode=glib-client "$<" > "$@"
