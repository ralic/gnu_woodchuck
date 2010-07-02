bin_PROGRAMS = smart-storage-logger ssl-tail smart-storage-simulator
# We only include the public key, obviously :-)
dist_pkgdata_DATA = ssl-receiver.cert

AM_CFLAGS = -Wall -g -O2 -std=gnu99 $(BASE_CFLAGS)
AM_CPPFLAGS = -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -DPKGDATADIR=\"$(pkgdatadir)\"
smart_storage_logger_CPPFLAGS = -DLOG_TO_DB $(AM_CPPFLAGS)
smart_storage_logger_LDFLAGS = 
smart_storage_logger_LDADD = -lpthread $(BASE_LIBS)
smart_storage_simulator_LDFLAGS =
smart_storage_simulator_LDADD = -lm $(BASE_LIBS)

ssl_tail_LDADD = $(BASE_LIBS)
ssl_tail_SOURCES = ssl-tail.c files.c files.h util.h

smart_storage_logger_SOURCES = smart-storage-logger.c \
	uploader.h uploader.c \
	files.h files.c \
	debug.h debug.c util.h sqlq.c sqlq.h \
	list.h btree.h btree.c \
	dbus-print-message.c dbus-print-message.h \
	glib.h \
	pidfile.h pidfile.c

smart_storage_simulator_SOURCES = smart-storage-simulator.c \
	debug.h debug.c util.h \
	list.h btree.h btree.c

