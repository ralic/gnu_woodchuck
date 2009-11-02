bin_PROGRAMS = smart-storage-logger

AM_CFLAGS = -Wall -g -O2 -std=gnu99
AM_CPPFLAGS = -D_GNU_SOURCE
AM_LDFLAGS = -lpthread

smart_storage_logger_SOURCES = smart-storage-logger.c \
	debug.h debug.c \
	list.h btree.h btree.c
