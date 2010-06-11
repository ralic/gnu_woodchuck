bin_PROGRAMS = smart-storage-logger smart-storage-simulator

AM_CFLAGS = -Wall -g -O2 -std=gnu99 $(BASE_CFLAGS)
AM_CPPFLAGS = -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
smart_storage_logger_LDFLAGS = 
smart_storage_logger_LDADD = -lpthread $(BASE_LIBS)
smart_storage_simulator_LDFLAGS =
smart_storage_simulator_LDADD = -lm $(BASE_LIBS)

smart_storage_logger_SOURCES = smart-storage-logger.c \
	debug.h debug.c util.h \
	list.h btree.h btree.c

smart_storage_simulator_SOURCES = smart-storage-simulator.c \
	debug.h debug.c util.h \
	list.h btree.h btree.c

