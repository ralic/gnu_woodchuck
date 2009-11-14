bin_PROGRAMS = smart-storage-logger smart-storage-simulator

AM_CFLAGS = -Wall -g -O2 -std=gnu99
AM_CPPFLAGS = -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
smart_storage_logger_LDFLAGS = -lpthread
smart_storage_simulator_LDFLAGS = -lm

smart_storage_logger_SOURCES = smart-storage-logger.c \
	debug.h debug.c util.h \
	list.h btree.h btree.c

smart_storage_simulator_SOURCES = smart-storage-simulator.c \
	debug.h debug.c util.h \
	list.h btree.h btree.c

