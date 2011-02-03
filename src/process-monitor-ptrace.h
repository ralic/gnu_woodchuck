/* process-monitor-ptrace.h - Process monitor interface.
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

#ifndef WOODCHUCK_PROCESS_MONITOR_PTRACE_H
#define WOODCHUCK_PROCESS_MONITOR_PTRACE_H

#include <sys/stat.h>
#include <stdbool.h>

/* The following callbacks must be implemented by the library's
   user.  */

enum wc_process_monitor_cbs
  {
    WC_PROCESS_OPEN_CB = 1,
    WC_PROCESS_CLOSE_CB,
    WC_PROCESS_UNLINK_CB,
    WC_PROCESS_RENAME_CB,
    WC_PROCESS_EXIT_CB,
  };

static inline const char *
wc_process_monitor_cb_str (enum wc_process_monitor_cbs cb)
{
  switch (cb)
    {
    case WC_PROCESS_OPEN_CB:
      return "open";
    case WC_PROCESS_CLOSE_CB:
      return "close";
    case WC_PROCESS_UNLINK_CB:
      return "unlink";
    case WC_PROCESS_RENAME_CB:
      return "rename";
    case WC_PROCESS_EXIT_CB:
      return "exit";
    case -1:
      /* Nothing to see here...  */
      return "free";
    default:
      return "unknown";
    }
}

struct wc_process_monitor_cb
{
  /* Which callback.  */
  enum wc_process_monitor_cbs cb;

  /* The PID that the user added explicitly via
     wc_process_monitor_ptrace_trace.  */
  int top_levels_pid;

  /* The thread id of the thread that actually performed the
     action.  */
  int tid;

  /* The exe, arg0 and arg1 of the thread that caused the event (not
     the top-level thread).  */
  const char *exe;
  const char *arg0;
  const char *arg1;

  union
  {
    struct
    {
      char *filename;
      /* The usual open flags.  */
      int flags;
      struct stat stat;
    } open;
    struct
    {
      char *filename;
      struct stat stat;
    } close;
    struct
    {
      char *filename;
      struct stat stat;
    } unlink;
    struct
    {
      char *src;
      char *dest;
      struct stat stat;
    } rename;
    struct
    {
    } exit;
  };
};

/* The user must implement this.  Called before the callback to
   determine whether a file is even required.  This will NOT be called
   from the main thread so be careful.  Becareful: FILENAME might be
   NULL.  */
extern bool process_monitor_filename_whitelisted (const char *filename);

extern void process_monitor_callback (struct wc_process_monitor_cb *cb);

/* Start the process monitor.  */
extern void wc_process_monitor_ptrace_init (void);

/* Stop tracing all processes.  */
extern void wc_process_monitor_ptrace_quit (void);

/* Trace process PID and any subsequent child processes.  PID must be
   the unix process id; it may not be a thread id.  */
extern bool wc_process_monitor_ptrace_trace (pid_t pid);

/* Stop tracing a previously traced process group.  Say you trace bash
   and it spawns emacs.  emacs will be traced and events will be
   reported as coming from the bash process group.  */
extern void wc_process_monitor_ptrace_untrace (pid_t pid);

#endif
