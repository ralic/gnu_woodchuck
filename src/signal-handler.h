/* signal-handler.h - Unix signal handler interface.
   Copyright 2011 Neal H. Walfield <neal@walfield.org>

   This file is part of Woodchuck.

   Woodchuck is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   Woodchuck is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.  */

#ifndef WOODCHUCK_SIGNAL_HANDLER_H
#define WOODCHUCK_SIGNAL_HANDLER_H

#include <glib-object.h>

#include <stdint.h>
#include <sys/signalfd.h>

#include "config.h"

/* Signal handler's interface.  */
typedef struct _WCSignalHandler WCSignalHandler;
typedef struct _WCSignalHandlerClass WCSignalHandlerClass;

#define WC_SIGNAL_HANDLER_TYPE (wc_signal_handler_get_type ())
#define WC_SIGNAL_HANDLER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), WC_SIGNAL_HANDLER_TYPE, WCSignalHandler))
#define WC_SIGNAL_HANDLER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), WC_SIGNAL_HANDLER_TYPE, WCSignalHandlerClass))
#define IS_WC_SIGNAL_HANDLER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), WC_SIGNAL_HANDLER_TYPE))
#define IS_WC_SIGNAL_HANDLER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), WC_SIGNAL_HANDLER_TYPE))
#define WC_SIGNAL_HANDLER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), WC_SIGNAL_HANDLER_TYPE, WCSignalHandlerClass))

struct _WCSignalHandlerClass
{
  GObjectClass parent;

  /* "unix-signal" signal: Called when a signal is caught.  Takes 1
     parameter, a struct signalfd_siginfo * (from <sys/signalfd.h>).  */
  guint unix_signal_signal_id;
};

extern GType wc_signal_handler_get_type (void);

/* Instantiate a idle monitor.  After instantiating, immediately
   connect to the object's "idle" and "disconnected" signals: existing
   connections will be created at the next "idle" point.  MASK is a
   set of signals to catch.  If NULL, then the empty set.  */
extern WCSignalHandler *wc_signal_handler_new (sigset_t *mask);

/* Add signal SIGNAL to the set of accepted signals (and add it to the
   thread's signal mask).  NB: If the program is multi-threaded, all
   relevenat signals should be added prior to starting any additional
   threads.  Otherwise, the thread's signal masks are not
   synchronized.  */
extern void wc_signal_handler_catch (WCSignalHandler *sh, int signal);

/* Like wc_signal_handler_catch, but takes a set of signals.  */
extern void wc_signal_handler_catch_mask (WCSignalHandler *sh, sigset_t *mask);

/* Ignore the signal SIGNAL.  NB: This module reference counts
   signals.  Thus a signal is only ignored if the number of times
   ignore is called matches the number of times catch is called.  NB:
   This function should not be called after additional threads have
   been started as all the threads' signal masks will not be
   synchronized.  */
extern void wc_signal_handler_ignore (WCSignalHandler *sh, int signal);

/* Like wc_signal_handler_ignore, but takes a set of signals.  */
extern void wc_signal_handler_ignore_mask (WCSignalHandler *sh, sigset_t *mask);

#endif
