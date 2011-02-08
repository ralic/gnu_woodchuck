/* process-monitor-ptrace.c - Process monitor.
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

#include "debug.h"

#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
/* At least for the N900, PTRACE_O_TRACESYSGOOD is not provided by
   <sys/ptrace.h>.  */
#include <linux/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <assert.h>
#include <linux/sched.h>
#include <pthread.h>
#include <sys/fcntl.h>
#include <limits.h>

#include "signal-handler.h"
#include "process-monitor-ptrace.h"

#include "util.h"

static int
tkill (int tid, int sig)
{
  return syscall (__NR_tkill, tid, sig);
}

/* A hash from pids to TCBs.  */
static GHashTable *tcbs;
static int tcb_count;

/* List of suspends threads as required to shed some load.  */
static GSList *suspended_tcbs;

struct load
{
  /* The number of callbacks and interesting events.  */
#define CALLBACK_COUNT_BUCKETS 10
  /* The minimum amount of time (in ms) that a bucket spans.  */
#define CALLBACK_COUNT_BUCKET_WIDTH 1000
  int callback_count[CALLBACK_COUNT_BUCKETS];
  int event_count[CALLBACK_COUNT_BUCKETS];
  /* The last time the callback count was reset.  */
  uint64_t callback_count_reset[CALLBACK_COUNT_BUCKETS];
  int callback_count_bucket;
};

/* A thread's control block.  */
struct tcb
{
  /* The thread's PID (or, rather, its tid).  */
  pid_t pid;

  struct load load;

  /* When we are notified that the thread performed a system call,
     ptrace does not indicated whether the thread is entering the
     system call or leaving it.  As system calls don't nest, we just
     need to know what the last event was.  On entry, we set this to
     the system call number and on exit we reset this to -1.  */
  long current_syscall;

  /* When a file is unlinked, we only want to generate an event if the
     unlink was successful.  We only know this on syscall exit.  At
     this point, we can no longer use the fd to get the full filename.
     To work around this, we stash the filename here.  The same goes
     for the stat buffer.  We have a similar problem when moving a
     file: once we move the file we can not get the absoluate file
     name of the full.  */
  void *saved_src;
  struct stat *saved_stat;

  /* Ptrace has a few help options.  This field indicates whether we
     have tried to set them and what the result was.  0: unintialized.
     1: set trace_options.  -1: initialization failed, but
     tracing.  */
  int trace_options;

  /* Typically, we don't want to just trace a thread, we want to trace
     all threads in that task.  This indicates whether we have scanned
     for the thread's siblings.  */
  bool scanned_siblings;

  /* We build a hierarchy of which thread started which thread
     (independent of task).  The reason is that the user wants to know
     what files some process accessed.  If it starts a sub-process and
     the user does not explicitly register that sub-process, then
     anything that that subprocess accesses should be attributed to
     the process.

     If the user adds this thread explicitly, parent is NULL.  */
  struct tcb *parent;
  GSList *children;

  /* We can't just stop tracing a thread at any time.  We have to wait
     until it has "yielded" to us.  To properly detach, we set this
     field to true and detach at the next opportunity.  */
  bool stop_tracing;

  /* A top-level thread is one that the user explicitly request we
     monitor.  When it, a sibling or a child does something, we send
     notify the user using this thread's pid.  */
  bool top_level;

  /* If a thread exits, it has children and is the user-visible handle
     (i.e., the user explicitly added it), we can't destroy it.  To
     remember that it is dead, we mark it as a zombie.  */
  bool zombie;

  /* If non-zero, we've decided to suspend the thread (for load
     shedding purposes).  If less then 0, then we need to suspend it
     at the next oppotunity.  If greater than 0, then it has been
     suspended.  The absolute value is the time we wanted to suspend
     it. */
  int64_t suspended;

  /* The executable and the first two arguments of the command
     line.  */
  char *exe;
  char *arg0;
  char *arg1;

  /* A file descriptor for accessing the thread's memory.  */
  int memfd;
  uint64_t memfd_lastuse;
};

static int memfd_count;

static pthread_t process_monitor_tid;

/* Events occur in the process monitor thread, which need to be
   forwarded to the user.  We need to make callbacks in the main
   thread.  We do this by using an idle handler (g_idle_add is thread
   safe).  */
static guint callback_id;

static pthread_mutex_t pending_callbacks_lock = PTHREAD_MUTEX_INITIALIZER;
static GSList *pending_callbacks;

static gboolean
callback_manager (gpointer user_data)
{
  assert (! pthread_equal (pthread_self (), process_monitor_tid));

  GSList *cbs;

  for (;;)
    {
      pthread_mutex_lock (&pending_callbacks_lock);

      cbs = pending_callbacks;
      pending_callbacks = NULL;

      if (! cbs)
	/* Only clear the signal id once we are done processing
	   everything.  */
	callback_id = 0;

      pthread_mutex_unlock (&pending_callbacks_lock);

      if (! cbs)
	/* Don't call again.  */
	return false;

      /* Make the callbacks in order.  */
      cbs = g_slist_reverse (cbs);
      while (cbs)
	{
	  struct wc_process_monitor_cb *cb = cbs->data;

	  debug (4, "Executing %p (%s)",
		 cb, wc_process_monitor_cb_str (cb->cb));

	  cbs = g_slist_delete_link (cbs, cbs);

	  if (cb->cb == -1)
	    /* Delayed free.  */
	    g_free (cb->open.filename);
	  else
	    process_monitor_callback (cb);

	  g_free (cb);
	}
    }
}

static void
callback_enqueue (struct tcb *tcb, int op,
		  const char *src,
		  const char *dest,
		  int flags,
		  struct stat *stat_buf)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  char *src_copy = NULL;

  struct wc_process_monitor_cb *cb;
  if (op == -1)
    /* Free.  Stash stat_buf in open.filename.  */
    {
      pthread_mutex_lock (&pending_callbacks_lock);
      if (! pending_callbacks)
	/* There are no pending callbacks, we can free this
	   already.  */
	{
	  pthread_mutex_unlock (&pending_callbacks_lock);
	  debug (4, "no pending callbacks. freeing %p immediately",
		 stat_buf);
	  g_free (stat_buf);
	  return;
	}
      else
	{
	  cb = g_malloc (sizeof (struct wc_process_monitor_cb));
	  cb->cb = op;
	  cb->open.filename = (void *) stat_buf;
	  src_copy = cb->open.filename;
	  goto enqueue_with_lock;
	}
    }

  int s_len = src ? strlen (src) + 1 : 0;
  int d_len = dest ? strlen (dest) + 1 : 0;

  cb = g_malloc (sizeof (struct wc_process_monitor_cb) + s_len + d_len);

  char *end = (void *) cb + sizeof (struct wc_process_monitor_cb);

  if (src)
    {
      src_copy = end;
      end = mempcpy (end, src, s_len);
    }

  char *dest_copy = NULL;
  if (dest)
    {
      dest_copy = end;
      end = mempcpy (end, dest, d_len);
    }

  cb->cb = op;

  struct tcb *tl = tcb;
  while (! tl->top_level)
    {
      assert (tl->parent);
      tl = tl->parent;
    }
  assert (! tl->parent);
  cb->top_levels_pid = tl->pid;

  cb->tid = tcb->pid;

  cb->exe = tcb->exe;
  cb->arg0 = tcb->arg0;
  cb->arg1 = tcb->arg1;

  if (! stat_buf)
    /* STAT_BUF may be NULL.  Don't seg fault.  */
    {
      stat_buf = alloca (sizeof (struct stat));
      memset (stat_buf, 0, sizeof (*stat_buf));
    }

  switch (op)
    {
    case WC_PROCESS_OPEN_CB:
      cb->open.filename = src_copy;
      cb->open.flags = flags;
      cb->open.stat = *stat_buf;
      break;
    case WC_PROCESS_CLOSE_CB:
      cb->close.filename = src_copy;
      cb->close.stat = *stat_buf;
      break;
    case WC_PROCESS_UNLINK_CB:
      cb->unlink.filename = src_copy;
      cb->unlink.stat = *stat_buf;
      break;
    case WC_PROCESS_RENAME_CB:
      cb->rename.src = src_copy;
      cb->rename.dest = dest_copy;
      cb->rename.stat = *stat_buf;
      break;
    case WC_PROCESS_EXIT_CB:
      break;
    }

  tcb->load.event_count[tcb->load.callback_count_bucket] ++;

  pthread_mutex_lock (&pending_callbacks_lock);
 enqueue_with_lock:
  /* Enqueue.  */
  debug (4, "Enqueuing %p: %d: "DEBUG_BOLD("%s")"(%d) (%s)",
	 cb, cb->tid, wc_process_monitor_cb_str (cb->cb), cb->cb, src_copy);

  pending_callbacks = g_slist_prepend (pending_callbacks, cb);

  if (callback_id == 0)
    /* Kick the idle handler.  */
    callback_id = g_idle_add (callback_manager, NULL);

  pthread_mutex_unlock (&pending_callbacks_lock);
}

/* Set TCB's parent to PARENT.  We sometimes have to do this after we
   create a thread because the initial SIGSTOPs and, indeed, some
   SIGTRAPs due to system calls can be delivered before the
   PTRACE_EVENT_CLONE event.  */
static void
tcb_parent_set (struct tcb *tcb, struct tcb *parent)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  if (tcb->parent == parent)
    return;

  if (! parent)
    {
      if (tcb->parent)
	{
	  tcb->parent->children = g_slist_remove (tcb->parent->children, tcb);
	  tcb->parent = NULL;
	}
      return;
    }

  /* We don't allow changing parents.  */
  assert (! tcb->parent);

  tcb->parent = parent;
  parent->children = g_slist_prepend (parent->children, tcb);
}

/* Remove a TCB from the TCBS hash and release the TCB data structure.
   The thread must already be detached.  The TCB must have no children
   and no parent.  */
static void
tcb_free (struct tcb *tcb)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  debug (4, "tcb_free (%d)", tcb->pid);

  assert (! tcb->parent);
  assert (! tcb->children);

  if (! g_hash_table_remove (tcbs, (gpointer) (uintptr_t) tcb->pid))
    {
      debug (0, "Failed to remove tcb for %d from hash table?!?",
	     (int) tcb->pid);
      assert (0 == 1);
    }

  if (tcb->memfd != -1)
    {
      close (tcb->memfd);
      memfd_count --;
    }

  if (tcb->suspended)
    suspended_tcbs = g_slist_remove (suspended_tcbs, tcb);

  /* We may still have pending callbacks.  These callbacks reference
     TCB->EXE.  To ensure we do not free TCB->EXE too early, we only
     free it after any pending callbacks.  (TCB->ARG0 and TCB->ARG1
     are allocated out of the same memory.)  */
  callback_enqueue (tcb, -1, NULL, NULL, 0, (void *) tcb->exe);
  g_free (tcb->saved_src);
  g_free (tcb->saved_stat);
  g_free (tcb);

  tcb_count --;
}

/* Read the TCB's executable and command line.  Normally done on
   start up and exec.  */
static void
tcb_read_exe (struct tcb *tcb)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  if (tcb->exe)
    callback_enqueue (tcb, -1, NULL, NULL, 0, (void *) tcb->exe);
  tcb->exe = tcb->arg0 = tcb->arg1 = NULL;

  char exe[256];
  snprintf (exe, sizeof (exe), "/proc/%d/exe", tcb->pid);
  int exe_len = readlink (exe, exe, sizeof (exe) - 1);
  if (exe_len < 0)
    {
      debug (0, "Failed to read link %s: %m", exe);
      exe_len = 0;
    }
  /* Add a NUL terminator.  */
  exe_len ++;
  exe[exe_len - 1] = 0;

  char *arg0 = NULL;
  int arg0len = 0;
  char *arg1 = NULL;
  int arg1len = 0;

  char cmdline[32];
  snprintf (cmdline, sizeof (cmdline), "/proc/%d/cmdline", tcb->pid);
  int fd = open (cmdline, O_RDONLY);
  char buffer[512];
  if (fd < 0)
    debug (0, "Error opening %s: %m\n", cmdline);
  else
    {
      int length = read (fd, buffer, sizeof (buffer));
      close (fd);

      if (length > 0)
	{
	  arg0 = buffer;
	  char *arg0end = memchr (arg0, 0, length);
	  if (! arg0end)
	    /* arg[0] appears to be truncated (Normally it is NUL
	       terminated).  */
	    {
	      arg0len = length;
	      if (length < sizeof (buffer))
		/* We don't need to truncate the last byte.  */
		arg0len ++;
	      arg0[arg0len - 1] = 0;
	    }
	  else
	    {
	      arg0len = (uintptr_t) arg0end - (uintptr_t) arg0 + 1;
	      length -= arg0len;

	      if (length > 0)
		{
		  arg1 = arg0end + 1;
		  char *arg1end = memchr (arg1, 0, length);

		  if (! arg1end)
		    /* arg[1] appears to be truncated.  */
		    {
		      arg1len = length;
		      if ((uintptr_t) arg1 + length
			  < (uintptr_t) buffer + sizeof (buffer))
			arg1len ++;
		      /* We don't need to truncate the last byte.  */
		      arg1[arg1len - 1] = 0;
		    }
		  else
		    arg1len = (uintptr_t) arg1end - (uintptr_t) arg1 + 1;
		}
	    }
	}
    }

  tcb->exe = g_malloc (exe_len + arg0len + arg1len);
  char *end = mempcpy (tcb->exe, exe, exe_len);
  if (arg0)
    {
      tcb->arg0 = end;
      end = mempcpy (tcb->arg0, arg0, arg0len);
    }
  if (arg1)
    {
      tcb->arg1 = end;
      end = mempcpy (tcb->arg1, arg1, arg1len);
    }
}

/* Start tracing PID.  Its parent is PARENT.  ALREADY_PTRACING
   indicates whether the thread is in trace mode or not.  */
static struct tcb *
thread_trace (pid_t pid, struct tcb *parent, bool already_ptracing)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  debug (3, "thread_trace (%d, %p (= %d), %s ptracing)",
	 (int) pid, parent, parent ? parent->pid : 0,
	 already_ptracing ? "already" : "not yet");

  struct tcb *tcb = g_hash_table_lookup (tcbs, (gpointer) (uintptr_t) pid);
  if (tcb)
    /* It's already being traced.  This happens in two cases: when we
       see the SIGSTOP before the PTRACE_EVENT_CLONE event; and, when
       the user explicitly traces a process that is already being
       traced.  */
    {
      assert (pid == tcb->pid);
      tcb_parent_set (tcb, parent);
      return tcb;
    }

  /* NB: After tracing a process, we first receive two SIGSTOPs
     (signal 0x13) for that process.  Subsequently, we receive
     SIGTRAPs on system calls.  */
  if (! already_ptracing)
    {
      if (ptrace (PTRACE_ATTACH, pid) == -1)
	{
	  debug (0, "Error attaching to %d: %m", pid);
	  return NULL;
	}
    }

  tcb = g_malloc0 (sizeof (struct tcb));
  tcb->pid = pid;

  tcb->memfd = -1;

  tcb->current_syscall = -1;

  tcb_parent_set (tcb, parent);

  g_hash_table_insert (tcbs, (gpointer) (uintptr_t) pid, tcb);
  tcb_count ++;

  tcb_read_exe (tcb);

  return tcb;
}

/* Stop tracing a thread.  The thread must already be detached, i.e.,
   it terminated or we called ptrace (PTRACE_DETACH, tcb->pid).  */
static void
thread_untrace (struct tcb *tcb)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  debug (4, "thread_untrace (%d)", tcb->pid);

  /* We need to reparent any children.  */
  if (tcb->children)
    {
      if (tcb->top_level)
	/* Whoops...  we are a top-level process: we need to stay
	   around as a zombie.  */
	{
	  assert (! tcb->parent);
	  tcb->zombie = true;
	  return;
	}

      /* Our parent inherits the children.  */
      assert (tcb->parent);

      GSList *l;
      for (l = tcb->children; l; l = l->next)
	{
	  struct tcb *c = l->data;
	  assert (c->parent == tcb);
	  assert (! c->top_level);

	  c->parent = tcb->parent;
	}

      tcb->parent->children
	= g_slist_concat (tcb->parent->children, tcb->children);
      tcb->children = NULL;
    }

  struct tcb *top_level = NULL;
  if (tcb->parent)
    /* Remove ourself from our parent.  */
    {
      assert (! tcb->top_level);

      tcb->parent->children = g_slist_remove (tcb->parent->children, tcb);

      if (! tcb->parent->children && tcb->parent->zombie)
	/* We were the last child and our parent is a zombie (which
	   implies it is a top-level thread).  We can free our
	   parent.  */
	{
	  assert (tcb->parent->top_level);
	  assert (! tcb->parent->parent);

	  top_level = tcb->parent;
	}

      tcb->parent = NULL;
    }
  else
    /* We are the top-level thread.  */
    {
      /* Actually, that may not be the case.  Consider the following:
	 a thread forks, we process the initial SIGSTOP and add it
	 (without its parent), it exits, and then we get the
	 PTRACE_EVENT_CLONE event.  */
      if (tcb->top_level)
	if (! tcb->children)
	  /* And, we have no children.  */
	  top_level = tcb;
    }

  if (top_level)
    /* We are freeing a top-level thread.  */
    {
      /* XXX: Signal the user.  */
      assert (top_level->top_level);
      assert (! top_level->parent);

      tcb_free (top_level);
    }

  if (top_level != tcb)
    tcb_free (tcb);
}

/* Start tracing a process.  Return the corresponding TCB for the
   thread group leader.

   Actually, we don't just trace the process: we also trace any child
   processes, which are created after we start tracing the process
   under the assumption that the program either has a multi-process
   architecture, uses plug-ins, etc.  As we attach to most processes
   shortly after they are created, we are unlikely to miss any child
   processes.  */
static struct tcb *
process_trace (pid_t pid)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  struct tcb *tcb = thread_trace (pid, NULL, false);
  assert (! tcb->top_level);
  tcb->top_level = true;
  return tcb;
}

/* Stop tracing a process and its children.  */
static void
process_untrace (pid_t pid)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  struct tcb *tcb = g_hash_table_lookup (tcbs, (gpointer) (uintptr_t) pid);
  if (! tcb)
    /* A user can untrace a PID more than once.  */
    {
      debug (0, "Can't untrace %d: not being traced.", pid);
      return;
    }

  if (! tcb->top_level)
    {
      assert (tcb->parent);
      debug (0, "Bad untrace: %d never explicitly traced.", pid);
      return;
    }
  else
    /* Top-level threads don't have parents.  */
    assert (! tcb->parent);

  if (tcb->stop_tracing)
    {
      debug (0, "Already untracing %d.", pid);
      return;
    }

  debug (4, "Will detach from %d (and children) at next opportunity.", pid);

  void stop (struct tcb *tcb)
  {
    if (tcb->suspended <= 0 && tkill (tcb->pid, SIGSTOP) < 0)
      debug (0, "tkill (%d, SIGSTOP): %m", pid);
    tcb->stop_tracing = true;

    GSList *l;
    for (l = tcb->children; l; l = l->next)
      {
	struct tcb *child = l->data;
	assert (child->parent == tcb);
	assert (! child->stop_tracing);
	assert (! child->top_level);
	stop (child);
      }
  }
  stop (tcb);
}

static bool quit;

enum process_monitor_commands
  {
    PROCESS_MONITOR_QUIT = 1,
    PROCESS_MONITOR_TRACE,
    PROCESS_MONITOR_UNTRACE,
  };

struct process_monitor_command
{
  enum process_monitor_commands command;
  /* Only valid for PROCESS_MONITOR_TRACE and PROCESS_MONITOR_UNTRACE.  */
  int pid;
};

static pthread_cond_t process_monitor_signaler_init_cond
  = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t process_monitor_commands_lock
  = PTHREAD_MUTEX_INITIALIZER;
static GSList *process_monitor_commands;

static int signal_process_pipe[2];

static pid_t signal_process_pid;

static void
process_monitor_signaler_init (void)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));
  assert (! signal_process_pid);

  if (pipe (signal_process_pipe) < 0)
    debug (0, DEBUG_BOLD ("Failed to create signal process pipe: %m"));

  pthread_mutex_lock (&process_monitor_commands_lock);
  switch ((signal_process_pid = fork ()))
    {
    case -1:
      debug (0, "Failed to fork process signal thread.");
      abort ();
    case 0:
      /* Child.  */
      debug (4, "Signal process monitor running.");
      while (true)
	{
	  int c;
	  int r = read (signal_process_pipe[0], &c, 1);
	  if (r < 0)
	    {
	      fprintf (stdout, "Failed to read from signal pipe: %m\n");
	      exit (1);
	    }
	}
    default:
      /* Parent.  */
      debug (3, "Signal process started, pid: %d", signal_process_pid);
      if (ptrace (PTRACE_ATTACH, signal_process_pid) == -1)
	debug (0, "Error attaching to %d: %m", signal_process_pid);

      pthread_cond_signal (&process_monitor_signaler_init_cond);
      pthread_mutex_unlock (&process_monitor_commands_lock);
    }
}

static void
process_monitor_command (enum process_monitor_commands command, pid_t pid)
{
  assert (! pthread_equal (pthread_self (), process_monitor_tid));

  if (quit)
    {
      debug (0, "Not queuing command: monitor already quit.");
      return;
    }

  assert (command == PROCESS_MONITOR_QUIT
	  || command == PROCESS_MONITOR_TRACE
	  || command == PROCESS_MONITOR_UNTRACE);

  struct process_monitor_command *cmd = g_malloc (sizeof (*cmd));
  cmd->command = command;
  cmd->pid = pid;

  pthread_mutex_lock (&process_monitor_commands_lock);
  process_monitor_commands = g_slist_prepend (process_monitor_commands, cmd);
  if (! process_monitor_commands->next)
    /* This is the only event.  */
    {
      debug (4, "Only event.  Signalling signal process.");
      int w;
      do
	w = write (signal_process_pipe[1], "", 1);
      while (w == 0);
      if (w < 0)
	{
	  debug (3, DEBUG_BOLD ("Error writing to signal process: %m"));
	  if (tkill (signal_process_pid, SIGCONT) < 0)
	    debug (0, "signalling signal process (%d): %m",
		   signal_process_pid);
	}
    }
  
  pthread_mutex_unlock (&process_monitor_commands_lock);
}

static void
memfd_cleanup (void)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  void iter (gpointer key, gpointer value, gpointer user_data)
  {
    struct tcb *tcb = value;
    uintptr_t horizon = (uintptr_t) user_data;

    if (tcb->memfd != -1 && tcb->memfd_lastuse < horizon)
      {
	close (tcb->memfd);
	tcb->memfd = -1;
	memfd_count --;
      }
  }

  uintptr_t n = now ();

  if (memfd_count > 96)
    /* Close any fd not used in the last 60 seconds.  */
    g_hash_table_foreach (tcbs, iter, (gpointer) (uintptr_t) (n - 60*1000));
  if (memfd_count > 128)
    /* That apparently didn't close enough.  Try again.  Close any fd
       not used in the last 5 seconds.  */
    g_hash_table_foreach (tcbs, iter, (gpointer) (uintptr_t) (n - 5*1000));
}

/* Extract a string (at most SIZE bytes long) from at address ADDR in
   thread PID's address space and save it in BUFFER.  If we have to
   fall back to ptrace, this becomes expensive because we can only
   read a word at a time.  Returns the location of the last character
   written.  Does not necessarily NUL terminate the string.  */
static char *
grab_string (struct tcb *tcb, uintptr_t addr, char *buffer, int size)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  assert (size > 0);
  char *b = buffer;
  *b = 0;

  char filename[32];
  snprintf (filename, sizeof (filename), "/proc/%d/mem", (int) tcb->pid);
  if (tcb->memfd == -1)
    {
      memfd_cleanup ();

      tcb->memfd = open (filename, O_RDONLY);
      if (tcb->memfd < 0)
	{
	  tcb->memfd = -1;
	  debug (0, "Failed to open %s: %m", filename);
	  goto try_ptrace;
	}
      memfd_count ++;
    }

  tcb->memfd_lastuse = now ();

  if (lseek (tcb->memfd, addr, SEEK_SET) < 0)
    {
      debug (0, "Error seeking in %s: %m", filename);
      goto try_ptrace;
    }

  int s = size;
  while (s > 0)
    {
      int r = read (tcb->memfd, b, s);
      if (r < 0)
	{
	  debug (0, "Error reading from %s: %m", filename);

	  if (buffer == b)
	    /* We didn't manage to read anything...  */
	    goto try_ptrace;

	  /* We got something.  Likely we are okay.  */
	  *b = 0;
	  return b;
	}

      char *end = memchr (b, 0, r);
      if (end)
	return end;

      s -= r;
    }

  /* B points at the next byte to write.  */
  return b - 1;
  
 try_ptrace:
  b = buffer;

  int offset = addr & (sizeof (uintptr_t) - 1);
  uintptr_t word = addr - offset;

  for (;;)
    {
      /* Clear ERRNO as we can't tell if ptrace failed (-1 implies an
	 error, but is a valid return value).  */
      errno = 0;
      union
      {
	uintptr_t word;
	char byte[0];
      } data = { .word = ptrace (PTRACE_PEEKDATA, tcb->pid, word, 0) };
      if (errno)
	{
	  debug (0, "Reading user's address space: %m.");
	  *b = 0;
	  return b;
	}

      int i;
      for (i = offset; i < sizeof (uintptr_t); i ++)
	{
	  *b = data.byte[i];
	  if (! *b)
	    return b;
	  size --;
	  if (size == 0)
	    return b;
	  b ++;
	}

      offset = 0;
      word += sizeof (uintptr_t);
    }
}

static struct load global_load;
static uint64_t global_load_check;

static void
load_shed_maybe (void)
{
  /* XXX: Make this do something useful.  */
  return;

  uint64_t n = now ();

  if (n - global_load_check < 2 * CALLBACK_COUNT_BUCKET_WIDTH)
    /* It's too soon.  */
    return;
  global_load_check = n;

  struct summary
  {
    int time;
    int callbacks;
    int events;
    int callbacks_per_sec;
  };
  struct summary summarize (const struct load *load)
  {
    struct summary summary = { 0, 0, 0, 0 };

    uint64_t earliest = n;
    int i;
    for (i = 0; i < CALLBACK_COUNT_BUCKETS; i ++)
      {
	summary.callbacks += load->callback_count[i];
	summary.events += load->event_count[i];
	if (load->callback_count_reset[i] < earliest)
	  earliest = load->callback_count_reset[i];
      }

    if (earliest == 0)
      earliest = n
	- 1000 * CALLBACK_COUNT_BUCKET_WIDTH * CALLBACK_COUNT_BUCKETS;

    summary.time = (int) (n - earliest);
    if (summary.time < 1000)
      /* Avoid division by 0.  */
      summary.time = 1000;

    if (summary.time > 1000)
      summary.callbacks_per_sec
	= (int) (summary.callbacks / (summary.time / 1000));

    return summary;
  }

  struct summary global = summarize (&global_load);
  if (global.time > (CALLBACK_COUNT_BUCKET_WIDTH * CALLBACK_COUNT_BUCKETS
		     + CALLBACK_COUNT_BUCKET_WIDTH / 2))
    /* We've not being activated continuously in the recent past.
       Don't do anything.  */
    return;

  if (global.callbacks_per_sec < 3000)
    /* A load of less than 3k callbacks per second is fine.  */
    return;

  debug (0, DEBUG_BOLD ("Need to shed some load: %d callbacks/s."),
	 (int) (global.callbacks / (global.time / 1000)));

  GSList *candidates = NULL;
  void iter (gpointer key, gpointer value, gpointer user_data)
  {
    // pid_t pid = (int) (uintptr_t) key;
    struct tcb *tcb = value;

    if (tcb->suspended)
      /* Already suspended.  */
      return;

    if (n - tcb->load.callback_count_reset[tcb->load.callback_count_bucket]
	> CALLBACK_COUNT_BUCKET_WIDTH * 2)
      /* It hasn't run recently.  */
      return;

    struct summary summary = summarize (&tcb->load);
    if (summary.events)
      /* It's producing interesting events...  */
      return;

    if (summary.time > (CALLBACK_COUNT_BUCKET_WIDTH * CALLBACK_COUNT_BUCKETS
			+ CALLBACK_COUNT_BUCKET_WIDTH / 2))
      /* It hasn't been doing much.  */
      return;

    if (summary.callbacks_per_sec > global.callbacks_per_sec / 5)
      /* At least 20% of the callbacks are due to this task...  */
      candidates = g_slist_prepend (candidates, tcb);
  }
  g_hash_table_foreach (tcbs, iter, NULL);

  if (! candidates)
    /* Tja...  */
    {
      debug (0, "Nothing to suspend.");
      return;
    }

  gint compare (gconstpointer a, gconstpointer b)
  {
    const struct tcb *x = a;
    const struct tcb *y = b;

    struct summary summary_x = summarize (&x->load);
    struct summary summary_y = summarize (&y->load);

    if (summary_x.callbacks_per_sec < summary_y.callbacks_per_sec)
      return -1;
    if (summary_x.callbacks_per_sec == summary_y.callbacks_per_sec)
      return 0;
    return 1;
  }
  candidates = g_slist_sort (candidates, compare);

  struct tcb *tcb = candidates->data;
  debug (0, DEBUG_BOLD ("Suspending %d: %s;%s;%s: %d callbacks per second"),
	 tcb->pid, tcb->exe, tcb->arg0, tcb->arg1,
	 summarize (&tcb->load).callbacks_per_sec);

  g_slist_free (candidates);

  tcb->suspended = -n;
  suspended_tcbs = g_slist_prepend (suspended_tcbs, tcb);
}

static void
load_increment (struct tcb *tcb)
{
  uint64_t n = now ();

  void update (struct load *load)
  {
    if (n - load->callback_count_reset[load->callback_count_bucket]
	> CALLBACK_COUNT_BUCKET_WIDTH)
      {
	load->callback_count_bucket ++;
	if (load->callback_count_bucket == CALLBACK_COUNT_BUCKETS)
	  load->callback_count_bucket = 0;

	load->callback_count_reset[load->callback_count_bucket] = n;
	load->callback_count[load->callback_count_bucket] = 1;
	load->event_count[load->callback_count_bucket] = 0;
      }
    else
      load->callback_count[load->callback_count_bucket] ++;
  }

  update (&tcb->load);
  update (&global_load);
}

/* Here's where all the magic happens.  */
static void *
process_monitor (void *arg)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  process_monitor_signaler_init ();

  while (! quit || tcb_count > 0)
    {
      /* SIGNO is the signal the child received and the signal to be
	 propagated to the child (if any).  */
      int signo = 0;
      struct tcb *tcb = NULL;

      /* Is there a race between checking for new commands and calling
	 waitpid?  No.  Consider the case that the process monitor
	 checks for new commands, finds none, drops the lock and then
	 is descheduled.  At this point, the user adds a new thread to
	 trace.  As the lock is free, the command is added to the
	 queued command list.  The process monitor now calls waitpid.
	 Because adding the command also tickled the wait process, we
	 will wake up immediately.  */
      int status = 0;
      pid_t pid = waitpid (-1, &status, __WALL|__WCLONE);
      if (pid == signal_process_pid)
	{
	  debug (4, "signal from signal process");

	  pthread_mutex_lock (&process_monitor_commands_lock);
	  GSList *commands = process_monitor_commands;
	  process_monitor_commands = NULL;
	  pthread_mutex_unlock (&process_monitor_commands_lock);

	  if (commands)
	    /* Execute the commands in the order received.  */
	    commands = g_slist_reverse (commands);

	  while (commands)
	    {
	      struct process_monitor_command *c = commands->data;
	      commands = g_slist_delete_link (commands, commands);

	      int command = c->command;
	      pid_t pid = c->pid;
	      g_free (c);

	      switch (command)
		{
		case PROCESS_MONITOR_QUIT:
		  /* Quit.  This is easier said than done.  We want to
		     exit gracefully.  That means detaching from all
		     threads.  But we can only detach from a thread
		     when we've captured it.  Is there a way to force
		     the thread to do something so we can capture it
		     or do we have to wait for a convenient moment?
		     IT APPEARS that we can send the process a SIGSTOP
		     and it gets woken up.  */
		  debug (1, "Quitting.  Need to detach from:");

		  bool have_one = false;
		  void iter (gpointer key, gpointer value, gpointer user_data)
		  {
		    pid_t pid = (int) (uintptr_t) key;
		    struct tcb *tcb = value;
		    debug (1, "%d: %s;%s;%s",
			   pid, tcb->exe, tcb->arg0, tcb->arg1);
		    if (tcb->suspended > 0)
		      /* Already suspended.  */
		      thread_untrace (tcb);
		    else if (tkill (pid, SIGSTOP) < 0)
		      {
			* (bool *) user_data = true;
			debug (0, "tkill (%d, SIGSTOP): %m", pid);
		      }
		  }
		  g_hash_table_foreach (tcbs, iter, (gpointer) &have_one);

		  quit = true;
		  break;

		case PROCESS_MONITOR_TRACE:
		  if (quit)
		    debug (0, "Not tracing %d: shutting down", pid);
		  else
		    process_trace (pid);
		  break;

		case PROCESS_MONITOR_UNTRACE:
		  process_untrace (pid);
		  break;

		default:
		  debug (0, "Bad process monitor command %d: "
			 "memory corruption?",
			 command);
		  break;
		}
	    }

	  /* Resume the signal process so it can be signaled
	     again.  */
	  goto out;
	}

      int event = status >> 16;

      do_debug (4)
	{
	  debug (0, "Signal from %d: status: %x", pid, status);
	  if (WIFEXITED (status))
	    debug (0, " Exited: %d", WEXITSTATUS (status));
	  if (WIFSIGNALED (status))
	    debug (0, " Signaled: %s (%d)",
		   strsignal (WTERMSIG (status)), WTERMSIG (status));
	  if (WIFSTOPPED (status))
	    debug (0, " Stopped: %s (%d)",
		   WSTOPSIG (status) == (SIGTRAP | 0x80)
		   ? "monitor SIGTRAP" : strsignal (WSTOPSIG (status)),
		   WSTOPSIG (status));
	  if (WIFCONTINUED (status))
	    debug (0, " continued");
	  debug (0, " ptrace event: %x", event);
	}

      /* Look up the thread.  */
      tcb = g_hash_table_lookup (tcbs, (gpointer) (uintptr_t) pid);
      if (! tcb)
	/* We aren't monitoring this process...  There are two
	   possibilities: either it is a new thread and the initial
	   SIGSTOP was delivered before the thread create event was
	   delivered (PTRACE_O_TRACECLONE).  This is not a problem:
	   the thread will be fully configured once we see that event.
	   The alternative is that another thread in this task started
	   a program and we just got in its way.  OUCH.  */
	{
	  if (WSTOPSIG (status) == (0x80 | SIGTRAP))
	    /* It's got the ptrace monitor bit set.  Assume it belongs
	       to us.  */
	    {
	      tcb = thread_trace (pid, NULL, true);
	      tcb->scanned_siblings = true;
	      tcb->trace_options = 1;
	    }
	  else
	    debug (3, "Got signal for %d, but not monitoring it!", pid);

	  if (! tcb)
	    goto out;
	}

      /* See if the child exited.  */
      if (WIFEXITED (status))
	{
	  debug (4, "%d exited: %d.", (int) pid, WEXITSTATUS (status));
	  thread_untrace (tcb);
	  continue;
	}

      if (WIFSIGNALED (status))
	{
	  debug (4, "%d exited due to signal: %s.",
		 (int) pid, strsignal (WTERMSIG (status)));
	  thread_untrace (tcb);
	  continue;
	}

      /* The child is not dead.  Process the event.  */

      /* Resource accounting.  */
      load_increment (tcb);

      if (! WIFSTOPPED (status))
	/* Ignore.  Not stopped.  */
	{
	  debug (4, "%d: Not stopped.", (int) tcb->pid);
	  goto out;
	}

      signo = WSTOPSIG (status);

      if (quit || tcb->stop_tracing)
	/* We are trying to exit or we want to detach from this
	   process.  */
	{
	  debug (4, "Detaching %d", (int) tcb->pid);
	  if (ptrace (PTRACE_DETACH, tcb->pid, 0, SIGCONT) < 0)
	    debug (0, "Detaching from %d failed: %m", tcb->pid);
	  thread_untrace (tcb);
	  continue;
	}

      if (tcb->suspended < 0)
	/* Suspend pending.  Do it now.  */
	{
	  if (ptrace (PTRACE_DETACH, tcb->pid, 0, SIGCONT) < 0)
	    debug (0, "Detaching from %d failed: %m", tcb->pid);
	  tcb->suspended = -tcb->suspended;
	  continue;
	}

      if (tcb->trace_options == 0 && signo == SIGSTOP)
	/* The thread is now running.  */
	{
	  if (ptrace (PTRACE_SETOPTIONS, pid, 0,
		      (PTRACE_O_TRACESYSGOOD|PTRACE_O_TRACECLONE
		       |PTRACE_O_TRACEFORK|PTRACE_O_TRACEEXEC)) == -1)
	    {
	      debug (0, "Warning: Setting ptrace options on %d: %m", pid);
	      tcb->trace_options = -1;
	    }
	  else
	    tcb->trace_options = 1;

	  if (! tcb->scanned_siblings)
	    /* Find sibling threads.  */
	    {
	      tcb->scanned_siblings = true;

	      char filename[32];
	      snprintf (filename, sizeof (filename),
			"/proc/%d/task", tcb->pid);
	      GError *error = NULL;
	      GDir *d = g_dir_open (filename, 0, &error);
	      if (! d)
		{
		  debug (0, "Unable to open %s to get sibling threads: %s",
			 filename, error->message);
		  g_free (error);
		  error = NULL;
		}
	      else
		{
		  /* We have to loop over the set of threads multiple
		     times: before we freeze all the threads, one of
		     them may have created some new threads!  If we
		     get through this once without adding a new
		     thread, then we should (hopefully) have them
		     all.  */
		  for (;;)
		    {
		      bool added_one = false;

		      const char *e;
		      while ((e = g_dir_read_name (d)))
			{
			  int t = atoi (e);
			  if (t)
			    {
			      struct tcb *tcb2
				= g_hash_table_lookup
				(tcbs, (gpointer) (uintptr_t) t);
			      if (! tcb2)
				/* We are not monitoring it yet.  */
				{
				  if ((tcb2 = thread_trace (t, tcb, false)))
				    {
				      tcb2->scanned_siblings = true;
				      added_one = true;
				    }
				}
			      else if (t != pid)
				debug (4, "Already monitoring %d", t);
			    }
			}

		      if (added_one)
			{
			  g_dir_rewind (d);
			  continue;
			}
		      else
			break;
		    }
		  g_dir_close (d);
		}
	    }

	  goto out;
	}

      /* We are interested in system calls and events, not signals.  A
	 system call is indicated by setting the signal to
	 0x80|SIGTRAP.  If this is not the case, just forward the
	 signal to the process.  */
      if (! ((tcb->trace_options == 1 && (signo == (0x80 | SIGTRAP) || event))
	     || (tcb->trace_options == -1 && (signo == SIGTRAP))))
	/* Ignore.  Not our signal.  */
	{
	  debug (4, "%d: ignoring and forwarding signal '%s' (%d)",
		 pid, strsignal (signo), signo);
	  goto out;
	}

      /* When we resume the process, don't do send it a signal.  */
      signo = 0;

      /* If EVENT is not 0, then we got an thread-create event.  */

      if (event)
	{
	  unsigned long msg = -1;
	  if (ptrace (PTRACE_GETEVENTMSG, pid, 0, (uintptr_t) &msg) < 0)
	    debug (0, "PTRACE_GETEVENTMSG(%d): %m", pid);

	  switch (event)
	    {
	    case PTRACE_EVENT_EXEC:
	      /* XXX: WTF?  If we don't catch exec events, ptrace
		 options are cleared on exec, e.g., SIGTRAP is sent
		 without the high-bit set.  If we do set it, the
		 options are inherited.  */
	      debug (4, "%d exec'd", (int) (pid_t) msg);
	      tcb_read_exe (tcb);
	      goto out;
	    case PTRACE_EVENT_CLONE:
	    case PTRACE_EVENT_FORK:
	      {
		/* Get the name of the new child.  */
		pid_t child = (pid_t) msg;
		debug (1, "New thread %d", (int) child);
		if (child)
		  {
		    /* Set up its TCB.  */
		    struct tcb *tcb2 = thread_trace ((pid_t) child, tcb, true);
		    tcb2->scanned_siblings = true;
		    /* It automatically gets the options set on its
		       parent.  */
		    tcb2->trace_options = tcb->trace_options;
		  }
		goto out;
	      }
	    default:
	      debug (0, "Unknown event %d, ignoring.", event);
	      goto out;
	    }
	}

      /* It has got to be a system call.  To determine which one, we
	 need the system call number.  If it is one we are interested
	 in examining, we also need its arguments.  Just get the
	 thread's register file and be done with it.  */

#ifdef __x86_64__
# define SYSCALL (regs.orig_rax)
# define ARG1 (regs.rdi)
# define ARG2 (regs.rsi)
# define ARG3 (regs.rdx)
# define ARG4 (regs.r10)
# define ARG5 (regs.r8)
# define ARG6 (regs.r9)
# define RET (regs.rax)
      struct user_regs_struct regs;
#elif __arm__
      /* This layout assumes that there are no 64-bit parameters.  See
	 http://lkml.org/lkml/2006/1/12/175 for the complications.  */

      /* r7 */
# define SYSCALL (regs.ARM_r7)
      /* r0..r6 */
# define ARG1 (regs.ARM_ORIG_r0)
# define ARG2 (regs.ARM_r1)
# define ARG3 (regs.ARM_r2)
# define ARG4 (regs.ARM_r3)
# define ARG5 (regs.ARM_r4)
# define ARG6 (regs.ARM_r5)
# define RET (regs.ARM_r0)
      struct user_regs regs;
#else
# error Not ported to your architecture.
#endif

      if (ptrace (PTRACE_GETREGS, pid, 0, &regs) < 0)
	{
	  debug (0, "%d: Failed to get thread's registers: %m", (int) pid);
	  goto out;
	}

      bool syscall_entry = tcb->current_syscall == -1;
      long syscall;
      if (syscall_entry)
	syscall = tcb->current_syscall = SYSCALL;
      else
	{
	  /* It would be nice to have:

	       assert (syscall == tcb->current_syscall)

	     Unfortunately, at least rt_sigreturn on x86-64 clobbers
	     the syscall number of syscall exit!  */
	  if (SYSCALL != tcb->current_syscall)
	    {
	      debug (4, "%d: warning syscall %ld entry "
		     "followed by syscall %ld!?!",
		     (int) pid, tcb->current_syscall, (long) SYSCALL);
	    }

	  syscall = tcb->current_syscall;
	  tcb->current_syscall = -1;
	}

      /* Given a file description, return the absolute filename
	 associated with the fd in BUFFER.  Always NUL termiantes the
	 filename, potentially truncating the filename.  */
      bool lookup_fd (pid_t pid, int fd, char *buffer, size_t size)
      {
	if (fd >= 0)
	  {
	    snprintf (buffer, size, "/proc/%d/fd/%d", pid, fd);
	    int ret = readlink (buffer, buffer, size - 1);
	    if (ret < 0)
	      {
		debug (0, "Failed to read %s: %m", buffer);
		return false;
	      }

	    buffer[ret] = 0;
	    return true;
	  }
	else
	  return false;
      }

      const char *syscall_str (long syscall)
      {
	switch (syscall)
	  {
	  case __NR_clone:
	    return "clone";
	  case __NR_open:
	    return "open";
	  case __NR_openat:
	    return "openat";
	  case __NR_close:
	    return "close";
	  case __NR_unlink:
	    return "unlink";
	  case __NR_unlinkat:
	    return "unlinkat";
	  case __NR_rmdir:
	    return "rmdir";
	  case __NR_rename:
	    return "rename";
	  case __NR_renameat:
	    return "renameat";
	  default:
	    return "unknown";
	  }
      }

      debug (4, "%d: %s (%ld) %s",
	     (int) pid, syscall_str (syscall), syscall,
	     syscall_entry ? "entry" : "exit");

      if (syscall_entry && syscall == __NR_clone)
	{
	  do_debug (4)
	    {
	      uintptr_t flags = ARG1;
	      debug (0, "%d clone (flags: %"PRIxPTR"); "
		     "signal mask: %"PRIxPTR"; "
		     "flags:%s%s%s"
		     "%s%s%s%s%s"
		     "%s%s%s%s%s"
		     "%s%s%s%s%s"
		     "%s%s%s%s%s",
		     (int) tcb->pid, flags,
		     flags & CSIGNAL,
		     (flags & CLONE_VM) ? " VM" : "",
		     (flags & CLONE_FS) ? " FS" : "",
		     (flags & CLONE_FILES) ? " FILES" : "",
		     (flags & CLONE_SIGHAND) ? " SIGHAND" : "",
		     (flags & CLONE_PTRACE) ? " PTRACE" : "",
		     (flags & CLONE_VFORK) ? " VFORK" : "",
		     (flags & CLONE_PARENT) ? " PARENT" : "",
		     (flags & CLONE_THREAD) ? " THREAD" : "",
		     (flags & CLONE_NEWNS) ? " NEWNS" : "",
		     (flags & CLONE_SYSVSEM) ? " SYSVSEM" : "",
		     (flags & CLONE_SETTLS) ? " SETTLS" : "",
		     (flags & CLONE_PARENT_SETTID) ? " P_SETTID" : "",
		     (flags & CLONE_CHILD_CLEARTID) ? " C_CLEARTID" : "",
		     (flags & CLONE_DETACHED) ? " DETACHED" : "",
		     (flags & CLONE_UNTRACED) ? " UNTRACED" : "",
		     (flags & CLONE_CHILD_SETTID) ? " C_SETTID" : "",
		     (flags & CLONE_STOPPED) ? " STOPPED" : "",
		     (flags & CLONE_NEWUTS) ? " NEWUTS" : "",
		     (flags & CLONE_NEWIPC) ? " NEWIPC" : "",
		     (flags & CLONE_NEWUSER) ? " NEWUSER" : "",
		     (flags & CLONE_NEWPID) ? " NEWPID" : "",
		     (flags & CLONE_NEWNET) ? " NEWNET" : "",
		     (flags & CLONE_IO) ? " IO" : "");
	    }
	}
      else if (! syscall_entry && syscall == __NR_clone)
	{
	  pid_t child_pid = (pid_t) RET;

	  if (tcb->trace_options != 1)
	    /* Had we set PTRACE_O_TRACECLONE, we would automatically
	       trace new children.  Since we didn't we try to
	       intercept clone calls.  */
	    {
	      struct tcb *child = thread_trace (child_pid, tcb, false);
	      if (child)
		/* Either its part of the same thread group or its a new
		   task, which only has one thread.  */
		child->scanned_siblings = true;
	    }
	}
      else if (! syscall_entry && (syscall == __NR_open
				   || syscall == __NR_openat))
	{
	  uintptr_t filename;
	  int flags;
	  mode_t mode;
	  if (syscall == __NR_open)
	    {
	      filename = ARG1;
	      flags = ARG2;
	      mode = ARG3;
	    }
	  else
	    {
	      filename = ARG2;
	      flags = ARG3;
	      mode = ARG4;
	    }

	  int fd = (int) RET;

	  int unhandled
	    = flags & ~(O_RDONLY|O_WRONLY|O_RDWR|O_CREAT|O_EXCL|O_TRUNC
			|O_NONBLOCK|O_LARGEFILE|O_DIRECTORY);
	  debug (4, "%s (%"PRIxPTR", %s%s%s%s%s%s%s%s (%x; %x unknown), "
		 "%x) -> %d",
		 syscall_str (syscall),
		 /* filename, flags, mode.  */
		 filename,
		 ((flags & O_RDONLY) == O_RDONLY) || (flags & O_RDWR)
		 ? "R" : "-",
		 ((flags & O_WRONLY) == O_WRONLY) || (flags & O_RDWR)
		 ? "W" : "-",
		 flags & O_CREAT ? "C" : "-",
		 flags & O_EXCL ? "X" : "-",
		 flags & O_TRUNC ? "T" : "-",
		 flags & O_NONBLOCK ? "N" : "B",
		 flags & O_LARGEFILE ? "L" : "-",
		 flags & O_DIRECTORY ? "D" : "-",
		 flags, 
		 unhandled,
		 (int) mode, fd);

	  char buffer[1024];
	  do_debug (5)
	    {
	      grab_string (tcb, ARG1, buffer, sizeof (buffer));
	      /* Ensure it is NUL terminated.  */
	      buffer[sizeof (buffer) - 1] = 0;
	      if (fd < 0)
		debug (0, "opening %s failed.", buffer);
	    }

	  if (lookup_fd (tcb->pid, fd, buffer, sizeof (buffer)))
	    {
	      debug (4, "%d: %s;%s;%s: %s (%s, %c%c) -> %d",
		     (int) tcb->pid, tcb->exe, tcb->arg0, tcb->arg1,
		     syscall_str (syscall),
		     buffer,
		     ((flags & O_RDONLY) == O_RDONLY) || (flags & O_RDWR)
		     ? 'r' : '-',
		     ((flags & O_WRONLY) == O_WRONLY) || (flags & O_RDWR)
		     ? 'W' : '-',
		     fd);

	      if (process_monitor_filename_whitelisted (buffer))
		{
		  struct stat s;
		  if (stat (buffer, &s) < 0)
		    {
		      debug (4, "Failed to stat %s: %m", buffer);
		      memset (&s, 0, sizeof (s));
		    }
		  callback_enqueue (tcb, WC_PROCESS_OPEN_CB,
				    buffer, NULL, flags, &s);
		}
	    }
	}
      else if (syscall_entry && syscall == __NR_close)
	{
	  int fd = (int) ARG1;

	  char buffer[1024];
	  if (lookup_fd (tcb->pid, fd, buffer, sizeof (buffer)))
	    /* There is little reason to believe that a close on a
	       valid file descriptor will fail.  Don't save and wait
	       for the exit.  Marshall now.  */
	    {
	      debug (4, "%d: %s: close (%ld) -> %s",
		     (int) tcb->pid, tcb->exe,
		     /* fd.  */
		     (long) ARG1, buffer);

	      if (process_monitor_filename_whitelisted (buffer))
		{
		  struct stat s;
		  if (stat (buffer, &s) < 0)
		    {
		      debug (4, "Failed to stat %s: %m", buffer);
		      memset (&s, 0, sizeof (s));
		    }
		  callback_enqueue (tcb, WC_PROCESS_CLOSE_CB,
				    buffer, NULL, 0, &s);
		}
	    }
	  else
	    debug (4, "%d: %s: close (%ld)",
		   (int) tcb->pid, tcb->exe,
		   /* fd.  */
		   (long) ARG1);

	}
      else if (syscall_entry
	       && (syscall == __NR_unlink
		   || syscall == __NR_unlinkat
		   || syscall == __NR_rmdir
		   || syscall == __NR_rename
		   || syscall == __NR_renameat))
	{
	  uintptr_t filename;
	  if (syscall == __NR_unlinkat || syscall == __NR_renameat)
	    filename = ARG2;
	  else
	    filename = ARG1;

	  char buffer[1024];
	  if (grab_string (tcb, filename, buffer, sizeof (buffer)))
	    {
	      buffer[sizeof (buffer) - 1] = 0;

	      char *p;
	      if (buffer[0] == '/')
		/* Absolute path.  */
		p = g_strdup_printf ("/proc/%d/root/%s", pid, buffer);
	      else if (syscall != __NR_renameat || ARG1 == AT_FDCWD)
		/* relative path.  */
		p = g_strdup_printf ("/proc/%d/cwd/%s", pid, buffer);
	      else
		/* renameat, relative path.  */
		p = g_strdup_printf ("/proc/%d/fd/%d/%s",
				     (int) pid, (int) ARG3, buffer);

	      tcb->saved_src = canonicalize_file_name (p);
	      g_free (p);

	      if (tcb->saved_src)
		{
		  tcb->saved_stat = g_malloc (sizeof (struct stat));
		  if (stat (tcb->saved_src, tcb->saved_stat) < 0)
		    debug (4, "Failed to stat %s: %m", tcb->saved_src);
		}
	    }

	  debug (4, "%d: %s;%s;%s: %s (%s)",
		 (int) tcb->pid, tcb->exe, tcb->arg0, tcb->arg1,
		 syscall_str (syscall), tcb->saved_src);
	}
      else if (! syscall_entry
	       && (syscall == __NR_unlink
		   || syscall == __NR_unlinkat
		   || syscall == __NR_rmdir))
	{
	  debug (4, "%d: %s;%s;%s: %s (%s) -> %d",
		 (int) tcb->pid, tcb->exe, tcb->arg0, tcb->arg1,
		 syscall_str (syscall), tcb->saved_src, (int) RET);

	  if ((int) RET >= 0
	      && process_monitor_filename_whitelisted (tcb->saved_src))
	    callback_enqueue (tcb, WC_PROCESS_UNLINK_CB,
			      tcb->saved_src, NULL, 0, tcb->saved_stat);
	  g_free (tcb->saved_src);
	  tcb->saved_src = NULL;
	  g_free (tcb->saved_stat);
	  tcb->saved_stat = NULL;
	}
      else if (! syscall_entry
	       && (syscall == __NR_rename || syscall == __NR_renameat))
	{
	  char *src = tcb->saved_src;
	  tcb->saved_src = NULL;
	  char *dest = NULL;

	  if ((int) RET >= 0)
	    {
	      uintptr_t filename;
	      if (syscall == __NR_renameat)
		filename = ARG4;
	      else
		filename = ARG2;

	      char buffer[1024];
	      if (grab_string (tcb, filename, buffer, sizeof (buffer)))
		{
		  buffer[sizeof (buffer) - 1] = 0;

		  char *p;
		  if (buffer[0] == '/')
		    /* Absolute path.  */
		    p = g_strdup_printf ("/proc/%d/root/%s", pid, buffer);
		  else if (syscall != __NR_renameat || ARG1 == AT_FDCWD)
		    /* relative path.  */
		    p = g_strdup_printf ("/proc/%d/cwd/%s", pid, buffer);
		  else
		    /* renameat, relative path.  */
		    p = g_strdup_printf ("/proc/%d/fd/%d/%s",
					 (int) pid, (int) ARG3, buffer);
		  dest = canonicalize_file_name (p);
		  g_free (p);
		}

	      debug (4, "%s (%s, %s) -> %d",
		     syscall_str (syscall), src, dest, (int) RET);

	      if (process_monitor_filename_whitelisted (src)
		  || process_monitor_filename_whitelisted (dest))
		callback_enqueue (tcb, WC_PROCESS_RENAME_CB,
				  src, dest, 0, tcb->saved_stat);
	    }

	  /* Clean up.  */
	  g_free (tcb->saved_stat);
	  tcb->saved_stat = NULL;

	  g_free (src);
	  g_free (dest);
	}

    out:
      load_shed_maybe ();

      debug (4, "ptrace(SYSCALL, %d, sig: %d)", pid, signo);
      if (ptrace (PTRACE_SYSCALL, pid, 0, (void *) (uintptr_t) signo) < 0)
	{
	  debug (0, "Resuming %d: %m", pid);
	  /* The process likely disappeared, perhaps violently.  */
	  if (tcb)
	    {
	      if (ptrace (PTRACE_DETACH, tcb->pid, 0, 0) < 0)
		debug (0, "Detaching pid %d failed: %m", tcb->pid);
	      thread_untrace (tcb);
	    }
	}
    }

  debug (0, DEBUG_BOLD ("Process monitor exited."));

  /* Kill the signal process.  */
  kill (signal_process_pid, SIGKILL);
  return NULL;
}

bool
wc_process_monitor_ptrace_trace (pid_t pid)
{
  assert (! pthread_equal (pthread_self (), process_monitor_tid));

  char filename[32];
  snprintf (filename, sizeof (filename),
	    "/proc/%d/status", pid);

#if !defined(NDEBUG) || defined(PROCESS_TRACER_STANDALONE)
  /* Check that the process is the thread group leader.  */
  {
    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (! g_file_get_contents (filename, &contents, &length, &error))
      {
	debug (0, "Error reading %s: %s; can't trace non-existent process",
	       filename, error->message);
	g_error_free (error);
	error = NULL;
	return false;
      }

    char *tgid_str = strstr (contents, "\nTgid:");
    tgid_str += 7;
    int tgid = atoi (tgid_str);
    g_free (contents);

    if (tgid != pid)
      {
	debug (0, "Can't add process %d: not the thread group leader (%d).",
	       pid, tgid);
	return false;
      }
  }
#endif

  process_monitor_command (PROCESS_MONITOR_TRACE, pid);

  return true;
}

void
wc_process_monitor_ptrace_untrace (pid_t pid)
{
  assert (! pthread_equal (pthread_self (), process_monitor_tid));
  process_monitor_command (PROCESS_MONITOR_UNTRACE, pid);
}

void
wc_process_monitor_ptrace_quit (void)
{
  assert (! pthread_equal (pthread_self (), process_monitor_tid));
  process_monitor_command (PROCESS_MONITOR_QUIT, 0);
}

#ifdef PROCESS_TRACER_STANDALONE
static GMainLoop *loop;
#endif

static void
unix_signal_handler (WCSignalHandler *sh, struct signalfd_siginfo *si,
		     gpointer user_data)
{
  debug (0, "Got signal %s.", strsignal (si->ssi_signo));

  if (si->ssi_signo == SIGTERM || si->ssi_signo == SIGINT
      || si->ssi_signo == SIGQUIT || si->ssi_signo == SIGHUP
      || si->ssi_signo == SIGSEGV || si->ssi_signo == SIGABRT)
    {
      debug (0, "Caught %s, quitting.", strsignal (si->ssi_signo));
      wc_process_monitor_ptrace_quit ();

#ifdef PROCESS_TRACER_STANDALONE
      g_main_loop_quit (loop);
#endif

      /* Wait for the monitor to quiesce.  */
      int err = pthread_join (process_monitor_tid, NULL);
      if (err)
	{
	  errno = err;
	  debug (0, "joining monitor thread: %m");
	}
    }
}

void
wc_process_monitor_ptrace_init (void)
{
  assert (! pthread_equal (pthread_self (), process_monitor_tid));

  /* The signals we are iterested in.  */
  sigset_t signal_mask;
  sigemptyset (&signal_mask);
  sigaddset (&signal_mask, SIGTERM);
  sigaddset (&signal_mask, SIGINT);
  sigaddset (&signal_mask, SIGQUIT);
  sigaddset (&signal_mask, SIGHUP);
  sigaddset (&signal_mask, SIGSEGV);
  sigaddset (&signal_mask, SIGABRT);

  WCSignalHandler *sh = wc_signal_handler_new (&signal_mask);

  g_signal_connect (G_OBJECT (sh), "unix-signal",
		    G_CALLBACK (unix_signal_handler), NULL);

  /* pid_t fits in a pointer.  */
  tcbs = g_hash_table_new (g_direct_hash, g_direct_equal);

  pthread_create (&process_monitor_tid, NULL, process_monitor, NULL);
  pthread_mutex_lock (&process_monitor_commands_lock);
  while (! signal_process_pid)
    pthread_cond_wait (&process_monitor_signaler_init_cond,
		       &process_monitor_commands_lock);
  pthread_mutex_unlock (&process_monitor_commands_lock);
}

#ifdef PROCESS_TRACER_STANDALONE
bool
process_monitor_filename_whitelisted (const char *filename)
{
  if (! filename)
    return false;

  static const char *filename_whitelist[] =
    {
      "/home/user",
      "/media",
      "/mnt"
    };

  if (! (filename[0] == '/' && (filename[1] == 'h' || filename[1] == 'm')))
    /* Fast check.  */
    goto out;

  int i;
  for (i = 0;
       i < sizeof (filename_whitelist) / sizeof (filename_whitelist[0]);
       i ++)
    {
      int len = strlen (filename_whitelist[i]);
      if (strncmp (filename, filename_whitelist[i], len) == 0
	  && (filename[len] == 0 || filename[len] == '/'))
	{
	  debug (3, "File %s is whitelisted.", filename);
	  return true;
	}
    }

 out:
  debug (3, "File %s is "DEBUG_BOLD("blacklisted")".", filename);

  return false;
}

void
process_monitor_callback (struct wc_process_monitor_cb *cb)
{
  char *src = NULL;
  char *dest = NULL;
  struct stat *stat = NULL;

  switch (cb->cb)
    {
    case WC_PROCESS_OPEN_CB:
      src = cb->open.filename;
      stat = &cb->open.stat;
      break;
    case WC_PROCESS_CLOSE_CB:
      src = cb->close.filename;
      stat = &cb->close.stat;
      break;
    case WC_PROCESS_UNLINK_CB:
      src = cb->unlink.filename;
      stat = &cb->unlink.stat;
      break;
    case WC_PROCESS_RENAME_CB:
      src = cb->rename.src;
      dest = cb->rename.dest;
      stat = &cb->unlink.stat;
      break;
    case WC_PROCESS_EXIT_CB:
      debug (0, "%d(%d): %s;%s;%s exited.",
	     cb->top_levels_pid, cb->tid, cb->exe, cb->arg0, cb->arg1);
      return;
    }

  debug (0, "%d(%d): %s;%s;%s: %s (%s%s%s, "BYTES_FMT")",
	 cb->top_levels_pid, cb->tid,
	 cb->exe, cb->arg0, cb->arg1,
	 wc_process_monitor_cb_str (cb->cb),
	 src, dest ? " -> " : "", dest ?: "",
	 BYTES_PRINTF (stat->st_size));
}

static int input_count;
static char input[1024];

static gboolean
stdin_handler (GIOChannel *source, GIOCondition condition, gpointer user_data)
{
  int r = read (STDIN_FILENO, &input[input_count],
		sizeof (input) - 1 - input_count);
  input_count += r;

  while (input_count > 0)
    {
      char *end;
      /* Check for a newline or a full buffer.  */
      if ((end = memchr (input, '\n', input_count))
	  || (input_count == sizeof (input) - 1
	      && (end = &input[sizeof (input) - 1], input_count ++)))
	{
	  *end = 0;
	  int len = (uintptr_t) end - (uintptr_t) input + 1;

	  /* Process the command.  */
	  char *c = input;
	  while (*c == ' ')
	    c ++;
	  if (*c && !(c[1] == ' ' || c[1] == '\0'))
	    {
	      printf ("Bad command.\n");
	      goto done;
	    }

	  int a = atoi (&c[1]);
	  switch (*c)
	    {
	    case 'q':
	      raise (SIGQUIT);
	      break;
	    case 'a':
	      if (! wc_process_monitor_ptrace_trace (a))
		printf ("Failed to trace %d\n", a);
	      break;
	    case 'd':
	      wc_process_monitor_ptrace_untrace (a);
	      break;
	    default:
	      printf ("Bad command (try: 'a PID', 'd PID', or 'q').\n");
	      break;
	    }

	done:
	  memmove (input, &input[len], input_count - len);
	  input_count -= len;
	}
      else
	break;
    }

  /* Call again.  */
  return TRUE;
}

int
main (int argc, char *argv[])
{
  g_type_init ();
  g_thread_init (NULL);

  loop = g_main_loop_new (NULL, FALSE);

  if (argc != 1 && argc != 2)
    {
      printf ("Usage: %s [PID]\n", argv[0]);
      return 1;
    }

  int pid = 0;
  if (argc == 2)
    {
      pid = atoi (argv[1]);
      if (! pid)
	{
	  printf ("Invalid pid specified: %s\n", argv[1]);
	  return 1;
	}
    }

  /* Start the process monitor.  */
  wc_process_monitor_ptrace_init ();
  if (pid)
    wc_process_monitor_ptrace_trace (pid);

  /* Start the command interpreter.  */
  GIOChannel *stdin_channel = g_io_channel_unix_new (STDIN_FILENO);
  g_io_add_watch (stdin_channel, G_IO_IN, stdin_handler, NULL);

  g_main_loop_run (loop);

  return 0;
}
#endif
