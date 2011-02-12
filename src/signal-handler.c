/* signal-handler.c - Unix signal handler.
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

#include "config.h"
#include "signal-handler.h"

#include <stdio.h>
#include <glib.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <sys/signalfd.h>

#include "debug.h"

struct _WCSignalHandler
{
  GObject parent;

  int sigfd;

  /* The number of times each signal has been added.  */
  int signals[_NSIG];
};

static gboolean
signal_handler (GIOChannel *source, GIOCondition condition, gpointer user_data)
{
  WCSignalHandler *sh = WC_SIGNAL_HANDLER (user_data);

  struct signalfd_siginfo siginfo;

  int r = read (sh->sigfd, &siginfo, sizeof (siginfo));
  if (r == 0)
    {
      printf ("Got end of file on signal fd.\n");
      exit (1);
    }
  if (r == -1)
    {
      printf ("Reading from signal fd: %m\n");
      exit (1);
    }
  assert (r == sizeof (siginfo));

  debug (0, DEBUG_BOLD ("Got unix signal %s"), strsignal (siginfo.ssi_signo));

  g_signal_emit (sh, WC_SIGNAL_HANDLER_GET_CLASS (sh)->unix_signal_signal_id,
		 0, &siginfo);

  /* Call again.  */
  return TRUE;
}

static void
refabricate (WCSignalHandler *sh)
{
  sigset_t signal_mask;
  sigset_t signal_unblock;
  sigemptyset (&signal_mask);
  sigemptyset (&signal_unblock);

  int i;
  for (i = 1; i < _NSIG; i ++)
    if (sh->signals[i])
      sigaddset (&signal_mask, i);

  bool init = false;
  if (sh->sigfd == -1)
    init = true;

  if ((sh->sigfd = signalfd (sh->sigfd, &signal_mask, 0)) < 0)
    debug (0, "signalfd(): %m");

  if (init)
    {
      if (fcntl (sh->sigfd, F_SETFD, FD_CLOEXEC) < 0)
	debug (0, "fcntl (sigfd, F_SETFD, FD_CLOEXEC): %m");

      GIOChannel *sig_channel = g_io_channel_unix_new (sh->sigfd);
      g_io_add_watch_full (sig_channel, G_PRIORITY_HIGH,
			   G_IO_IN, signal_handler, sh, NULL);
    }

  /* Block them.  NB: The signal mask is only inherited by new
     threads.  */
  if (pthread_sigmask (SIG_SETMASK, &signal_mask, NULL) < 0)
    debug (0, "pthread_sigmask(): %m");
}

void
wc_signal_handler_catch (WCSignalHandler *sh, int signal)
{
  assert (signal > 0);
  assert (signal < _NSIG);

  sh->signals[signal] ++;
  if (sh->signals[signal] == 1)
    /* It's new.  */
    refabricate (sh);
}

void
wc_signal_handler_ignore (WCSignalHandler *sh, int signal)
{
  assert (signal > 0);
  assert (signal < _NSIG);

  if (sh->signals[signal] == 0)
    {
      debug (0, "Ignoring signal %d, but it is not being watched.",
	     signal);
      return;
    }

  sh->signals[signal] --;
  if (sh->signals[signal] == 0)
    /* It's new.  */
    refabricate (sh);
}

void
wc_signal_handler_catch_mask (WCSignalHandler *sh, sigset_t *mask)
{
  bool need_refab = false;

  int i;
  for (i = 1; i < _NSIG; i ++)
    if (sigismember (mask, i))
      {
	sh->signals[i] ++;
	if (sh->signals[i] == 1)
	  /* It's new.  */
	  need_refab = true;
      }

  if (need_refab)
    refabricate (sh);
}

void
wc_signal_handler_ignore_mask (WCSignalHandler *sh, sigset_t *mask)
{
  bool need_refab = false;

  int i;
  for (i = 1; i < _NSIG; i ++)
    if (sigismember (mask, i))
      {
	if (sh->signals[i] == 0)
	  {
	    debug (0, "Ignoring signal %d, but it is not being watched.", i);
	    continue;
	  }

	sh->signals[i] --;
	if (sh->signals[i] == 0)
	  /* It's new.  */
	  need_refab = true;
      }

  if (need_refab)
    refabricate (sh);
}

WCSignalHandler *
wc_signal_handler_new (sigset_t *mask)
{
  /* The signal handler service is a singleton.  */
  static WCSignalHandler *wc_signal_handler;

  if (wc_signal_handler)
    g_object_ref (wc_signal_handler);
  else
    wc_signal_handler
      = WC_SIGNAL_HANDLER (g_object_new (WC_SIGNAL_HANDLER_TYPE, NULL));

  if (mask)
    wc_signal_handler_catch_mask (wc_signal_handler, mask);

  return wc_signal_handler;
}

static void wc_signal_handler_dispose (GObject *object);

G_DEFINE_TYPE (WCSignalHandler, wc_signal_handler, G_TYPE_OBJECT);

static void
wc_signal_handler_class_init (WCSignalHandlerClass *klass)
{
  wc_signal_handler_parent_class = g_type_class_peek_parent (klass);

  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = wc_signal_handler_dispose;

  WCSignalHandlerClass *sh_class = WC_SIGNAL_HANDLER_CLASS (klass);
  sh_class->unix_signal_signal_id
    = g_signal_new ("unix-signal",
		    G_TYPE_FROM_CLASS (klass),
		    G_SIGNAL_RUN_FIRST,
		    0, NULL, NULL,
		    g_cclosure_marshal_VOID__POINTER,
		    G_TYPE_NONE, 1,
		    G_TYPE_POINTER);
}

static void
wc_signal_handler_init (WCSignalHandler *sh)
{
  sh->sigfd = -1;
}

static void
wc_signal_handler_dispose (GObject *object)
{
  g_critical ("Attempt to dispose signal handler singleton!");
}
