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
#include <sys/mman.h>

#include "signal-handler.h"
#include "process-monitor-ptrace.h"

#include "util.h"

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

static int
tkill (int tid, int sig)
{
  return syscall (__NR_tkill, tid, sig);
}

static char *
tid_state (pid_t tid, const char *key)
{
  char filename[32];
  gchar *contents = NULL;
  gsize length = 0;
  GError *error = NULL;
  snprintf (filename, sizeof (filename),
	    "/proc/%d/status", tid);
  if (! g_file_get_contents (filename, &contents, &length, &error))
    {
      debug (0, "Error reading %s: %s; can't trace non-existent process",
	     filename, error->message);
      g_error_free (error);
      error = NULL;
      return NULL;
    }

  char *record;
  do
    record = strstr (contents, key);
  while (record && ((record > contents && record[-1] != '\n')
		    || record[strlen (key)] != ':'));

  char *value = NULL;
  if (record)
    {
      record += strlen (key) + 1 /* Skip the colon as well.  */;
      while (*record == '\t' || *record == ' ')
	record ++;
      char *end = strchr (record, '\n');
      if (end)
	value = strndup (record, (uintptr_t) end - (uintptr_t) record);
    }
  else
    debug (0, "Field %s not present in %s!", key, filename);

  g_free (contents);

  debug (5, "%s -> %s", key, value);

  return value;
}

/* Given a Linux thread id, return the thread id of its process's
   group leader.  */
static pid_t
tid_to_process_group_leader (pid_t tid)
{
  char *s = tid_state (tid, "Tgid");
  if (! s)
    return 0;

  pid_t p = atoi (s);
  free (s);
  return p;
}

/* Given a Linux thread id, return the parent pid.  */
static pid_t
tid_to_ppid (pid_t tid)
{
  char *s = tid_state (tid, "PPid");
  if (! s)
    return 0;

  pid_t p = atoi (s);
  free (s);
  return p;
}

/* Binary instrumentation.  */

/* When ptracing a process, all signals and, optionally, all system
   calls are redirected to the debugger.  Intercepting every system
   call is expensive, particularly if we are only interested in
   examining a small fraction of all system calls.  In our case, we
   are interested in the relatively infrequent open, close,
   etc. system calls.

   To selectively intercept system calls, we instrument binaries, in
   particular, the libraries that actually make the system calls.  We
   search the binary for syscall signatures and, if it looks like it
   might be an interesting system call, we insert a break point.  When
   the program executes the break point, it is suspended and we
   receive a SIGTRAP.  To resume the thread, we need to fix up its
   state: we need to emulate the instruction we replaced and advance
   the thread's IP beyond the break point instruction.  */

/* The patches for each library.  */
struct library_patch
{
  /* A prefix of the library's filename, as it appears in
     /proc/pid/maps.  Any trailing characters from the set
     "1234567890-.so" are acceptable.  */
  char *filename;
  /* An array of patches.  */
  struct patch *patches;
  int patch_count;
  /* The size of the binary's executable section.  We assume that each
     binary has at most one executable section.  */
  uintptr_t size;
};

/* The maximum instruction length, in bytes.  */
#ifdef __arm__
# define INSTRUCTION_LEN_MAX 4
#else
# define INSTRUCTION_LEN_MAX 8
#endif

struct patch
{
  /* The offset of the fixup in library's executable section (in
     bytes).  */
  uintptr_t base_offset;

  /* The system call.  */
  long syscall;

  /* The length of the replaced instruction, in bytes.  */
  int ins_len;
  /* The value of the replaced instruction.  */
  char ins[INSTRUCTION_LEN_MAX];
};

/* The libraries we are interested in.  This is a static list, because
   there are very few libraries that actually make system calls.
   Because we only instrument these libraries, we might have false
   negatives, i.e., we'll miss some interesting system calls.  That's
   okay.  There are very few such binaries.  */
struct library_patch library_patches[] =
  {
#define LIBRARY_LD 0
    { "/lib/ld-", },
    { "/lib/libc-", },
    { "/lib/libpthread-", },
  };
#define LIBRARY_COUNT (sizeof (library_patches) / sizeof (library_patches[0]))

/* Load management.  */

/* We monitor the tracing overhead of each thread.  If the load
   appears to be too high, we can suspend tracing the thread.  */

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

/* Thread and process management.  */

#define TID_FMT "%d %s;%s;%s"
#define TID_PRINTF(tcb) \
  (tcb)->tid, (tcb)->pcb->exe, (tcb)->pcb->arg0, (tcb)->pcb->arg1 

/* A thread's control block.  One for each linux thread that we
   trace.  */
struct tcb
{
  /* The thread's thread id.  */
  pid_t tid;

  /* Its process.  */
  struct pcb *pcb;

  /* This thread's load.  */
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
  char *saved_src;
  struct stat *saved_stat;

  /* Ptrace has a few helpful options.  This field indicates whether
     we have tried to set them yet.  0: unintialized.  1: successfully
     set the options.  */
  int trace_options;

  /* We can't just stop tracing a thread at any time.  We have to wait
     until it has "yielded" to us.  To properly detach, we set this
     field to true and detach at the next opportunity.  */
  bool stop_tracing;

  /* If non-zero, we've decided to suspend the thread (for load
     shedding purposes).  If less then 0, then we need to suspend it
     at the next oppotunity.  If greater than 0, then it has been
     suspended.  The absolute value is the time we wanted to suspend
     it. */
  int64_t suspended;

  /* A file descriptor for accessing the process's memory.  */
  int memfd;
  /* The last time the memfd was used.  */
  uint64_t memfd_lastuse;
};

/* A hash from tids to struct tcb *s.  */
static GHashTable *tcbs;
static int tcb_count;

/* List of currently suspended threads (those threads we suspended to
   reduce the load).  */
static GSList *suspended_tcbs;

/* A process's control block.  All threads in the same process are
   mapped to the same pcb.  */
struct pcb
{
  /* The tcb of the process's group leader.  */
  struct tcb group_leader;

  /* The list of all threads in this process (including the group
     leader).  */
  GSList *tcbs;

  /* Typically, we don't want to just trace a thread, we want to trace
     all threads in that task.  This indicates whether we have scanned
     for the thread's siblings.  */
  bool scanned_siblings;

  /* We build a hierarchy of which process started which process.  The
     reason is that the user wants to know what files some process
     accessed.  If it starts a sub-process and the user does not
     explicitly register that sub-process, then anything that that
     subprocess accesses should be attributed to the process.  */
  struct pcb *parent;
  GSList *children;

  /* A top-level thread is one that the user explicitly requested we
     monitor.  When it, a sibling, or a child does something, we
     notify the user and include this thread's pid.  */
  bool top_level;

  /* If a process exits, it has children and is the user-visible
     handle (i.e., the user explicitly added it), we can't destroy it.
     To remember that it is dead, we mark it as a zombie.  */
  bool zombie;

  /* The executable and the first two arguments of the command
     line.  */
  char *exe;
  char *arg0;
  char *arg1;

  /* The fd used to open the various libraries.  */
  int lib_fd[LIBRARY_COUNT];
  /* Location of ld, libc and libpthread in the process's address space.  */
  uintptr_t lib_base[LIBRARY_COUNT];
};

/* A hash from pids to PCBs.  */
static GHashTable *pcbs;
static int pcb_count;

/* The number of open file descriptions to process' memory.  */
static int memfd_count;

/* The pthread id of the process monitor thread (the thread that does
   the actually ptracing.  */
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
  /* Executed in the context of the main thread.  */
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
  cb->timestamp = now ();

  /* Find the top-level process.  */
  struct pcb *tl = tcb->pcb;
  while (! tl->top_level)
    {
      assert (tl->parent);
      tl = tl->parent;
    }

  cb->top_levels_pid = tl->group_leader.tid;
  cb->top_levels_exe = tl->exe;
  cb->top_levels_arg0 = tl->arg0;
  cb->top_levels_arg1 = tl->arg1;

  cb->actor_pid = tcb->pcb->group_leader.tid;
  cb->actor_exe = tcb->pcb->exe;
  cb->actor_arg0 = tcb->pcb->arg0;
  cb->actor_arg1 = tcb->pcb->arg1;

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
    case WC_PROCESS_TRACING_CB:
      cb->tracing.added = flags;
      break;
    }

  tcb->load.event_count[tcb->load.callback_count_bucket] ++;

  pthread_mutex_lock (&pending_callbacks_lock);
 enqueue_with_lock:
  /* Enqueue.  */
  debug (4, "Enqueuing %p: %d: %s(%d) (%s)",
	 cb, cb->top_levels_pid,
	 wc_process_monitor_cb_str (cb->cb), cb->cb, src_copy);

  pending_callbacks = g_slist_prepend (pending_callbacks, cb);

  if (callback_id == 0)
    /* Kick the idle handler.  */
    callback_id = g_idle_add (callback_manager, NULL);

  pthread_mutex_unlock (&pending_callbacks_lock);
}

/* Set PCB's parent to PARENT.  We sometimes have to do this after we
   create a thread because the initial SIGSTOPs and, indeed, some
   SIGTRAPs due to system calls can be delivered before the
   PTRACE_EVENT_CLONE event.  */
static void
pcb_parent_set (struct pcb *pcb, struct pcb *parent)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  assert (parent);
  if (pcb->parent)
    {
      assert (parent == pcb->parent);
      return;
    }

  pcb->parent = parent;
  parent->children = g_slist_prepend (parent->children, pcb);
}

/* Remove a PCB from the PCBS hash and release the PCB data structure.
   All the process's threads must be freed.  */
static void
pcb_free (struct pcb *pcb)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  debug (4, "pcb_free (%d)", pcb->group_leader.tid);

  /* We better have no threads.  */
  assert (! pcb->tcbs);

  /* Free PCB and any of its children.  */
  void do_free (struct pcb *pcb)
  {
    assert (! pcb->children);
    assert (! pcb->tcbs);

    if (! g_hash_table_remove (pcbs,
			       (gpointer) (uintptr_t) pcb->group_leader.tid))
      {
	debug (0, "Failed to remove pcb for %d from hash table?!?",
	       (int) pcb->group_leader.tid);
	assert (0 == 1);
      }

    if (pcb->top_level)
      callback_enqueue (&pcb->group_leader,
			WC_PROCESS_EXIT_CB, NULL, NULL, 0, NULL);

    /* We may still have pending callbacks.  These callbacks reference
       PCB->EXE.  To ensure we do not free PCB->EXE too early, we only
       free it after any pending callbacks.  (PCB->ARG0 and PCB->ARG1
       are allocated out of the same memory.)  */
    callback_enqueue (&pcb->group_leader,
		      -1, NULL, NULL, 0, (void *) pcb->exe);

    struct pcb *p = NULL;
    if (pcb->parent)
      /* Remove ourself from our parent.  */
      {
	pcb->parent->children = g_slist_remove (pcb->parent->children, pcb);

	if (! pcb->parent->children && pcb->parent->zombie)
	  /* We are our parent's last descendant and it is a zombie.
	     Free it.  */
	  {
	    assert (pcb->parent->top_level);
	    p = pcb->parent;
	  }
      }

    g_free (pcb);
    pcb_count --;

    if (p)
      do_free (p);
  }

  /* We need to reparent any children.  */
  if (pcb->children)
    {
      if (pcb->top_level)
	/* Whoops...  we are a top-level process: we need to stay
	   around as a zombie.  */
	{
	  pcb->zombie = true;
	  return;
	}

      /* We are not a top-level.  We can exit now.  Our parent
	 inherits our children.  */

      assert (pcb->parent);
      GSList *l;
      for (l = pcb->children; l; l = l->next)
	{
	  struct pcb *child = l->data;
	  assert (child->parent == pcb);

	  child->parent = pcb->parent;
	}

      pcb->parent->children
	= g_slist_concat (pcb->parent->children, pcb->children);
      pcb->children = NULL;
    }

  do_free (pcb);

  debug (4, "%d processes still being traced (%d threads)",
	 pcb_count, tcb_count);
}

/* Read the PCB's executable and command line.  Normally done on
   start up and exec.  */
static void
pcb_read_exe (struct pcb *pcb)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  if (pcb->exe)
    /* Free it.  */
    callback_enqueue (&pcb->group_leader,
		      -1, NULL, NULL, 0, (void *) pcb->exe);
  pcb->exe = pcb->arg0 = pcb->arg1 = NULL;

  char exe[256];
  snprintf (exe, sizeof (exe), "/proc/%d/exe", pcb->group_leader.tid);
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
  snprintf (cmdline, sizeof (cmdline), "/proc/%d/cmdline",
	    pcb->group_leader.tid);
  int fd = open (cmdline, O_RDONLY);
  char buffer[512];
  if (fd < 0)
    debug (0, "Error opening %s: %m\n", cmdline);
  else
    {
      int length = read (fd, buffer, sizeof (buffer));
      close (fd);

      /* Arguments are separated by NUL terminators.  */
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

  pcb->exe = g_malloc (exe_len + arg0len + arg1len);
  char *end = mempcpy (pcb->exe, exe, exe_len);
  if (arg0)
    {
      pcb->arg0 = end;
      end = mempcpy (pcb->arg0, arg0, arg0len);
    }
  if (arg1)
    {
      pcb->arg1 = end;
      end = mempcpy (pcb->arg1, arg1, arg1len);
    }
}

static void
tcb_memfd_cleanup (void)
{
  assert (memfd_count >= 0);
  assert (memfd_count <= tcb_count);

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

static int
tcb_memfd (struct tcb *tcb, bool reopen)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  tcb->memfd_lastuse = now ();

  if (tcb->memfd != -1 && reopen)
    {
      close (tcb->memfd);
      tcb->memfd = -1;
      memfd_count --;
    }

  if (tcb->memfd == -1)
    {
      tcb_memfd_cleanup ();

      char filename[32];
      snprintf (filename, sizeof (filename), "/proc/%d/mem", (int) tcb->tid);
      tcb->memfd = open (filename, O_RDONLY);
      if (tcb->memfd < 0)
	{
	  debug (0, "Failed to open %s: %m", filename);
	  tcb->memfd = -1;
	  return -1;
	}
      memfd_count ++;
    }

  return tcb->memfd;
}

/* Read up to SIZE bytes from a process PCB's memory starting at
   address ADDR into buffer BUFFER.  If STRING is true, stop reading
   if we see a NUL byte.  Returns the location of the last byte
   written.  On error, returns NULL.  */
static char *
tcb_mem_read (struct tcb *tcb, uintptr_t addr, char *buffer, int size,
	      bool string)
{
  if (size == 0)
    return buffer;

  bool retry = false;
 do_retry:;
  int memfd = tcb_memfd (tcb, retry);
  if (memfd == -1)
    return NULL;

  if (lseek (memfd, addr, SEEK_SET) < 0)
    {
      debug (0, "Error seeking in process %d's memory: %m",
	     tcb->pcb->group_leader.tid);
      if (! retry)
	{
	  retry = true;
	  goto do_retry;
	}
      return NULL;
    }

  char *b = buffer;
  int s = size;
  while (s > 0)
    {
      int r = read (memfd, b, s);
      if (r < 0)
	{
	  if (buffer == b && ! retry)
	    {
	      retry = true;
	      goto do_retry;
	    }

	  debug (0, "Error reading from process %d's memory: %m",
		 tcb->pcb->group_leader.tid);

	  if (buffer == b)
	    /* We didn't manage to read anything...  */
	    return NULL;

	  /* We got something.  Likely we are okay.  */
	  break;
	}

      if (string)
	{
	  char *end = memchr (b, 0, r);
	  if (end)
	    return end;
	}

      s -= r;
      b += r;
    }

  /* B points at the next byte to write.  */
  return b - 1;
}

/* Returns whether the process has been patched.  */
static bool
pcb_patched (struct pcb *pcb)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  /* Whenever a new relevant library is loaded, we parse it.  Until it
     is loaded, no system calls from it can be made.  A process is
     thus patched once we have at least patched the loader.  */
  return pcb->lib_base[LIBRARY_LD] != 0
    && library_patches[LIBRARY_LD].patch_count;
}

/* Returns 0 if ADDR does not contain VALUE.  1 if it does.  -errno if
   an error occurs.  */
static int
thread_check (struct tcb *tcb, uintptr_t addr, const char *value, int bytes)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  errno = EFAULT;
  char real[bytes];
  if (! tcb_mem_read (tcb, addr, real, bytes, false))
    {
      debug (0, "Failed to read from process %d's memory: %m",
	     (int) tcb->pcb->group_leader.tid);
      return -errno;
    }

  if (memcmp (real, value, bytes) != 0)
    {
      GString *s = g_string_new ("");
      g_string_append_printf
	(s, DEBUG_BOLD ("Mismatch: reading process %d's memory")
	 ": at %"PRIxPTR": expected:",
	 (int) tcb->pcb->group_leader.tid, addr);
      int i;
      for (i = 0; i < bytes; i ++)
	g_string_append_printf (s, " 0x%02x", (int) (unsigned char) value[i]);
      g_string_append_printf (s, "; actual:");
      for (i = 0; i < bytes; i ++)
	g_string_append_printf (s, " 0x%02x", (int) (unsigned char) real[i]);
      debug (0, "%s", s->str);
      g_string_free (s, true);

      return 0;
    }
  else
    return 1;
}

static bool
thread_mem_update (struct tcb *tcb, uintptr_t addr,
		   const char *new_value, int bytes)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  int offset = addr & (sizeof (uintptr_t) - 1);
  uintptr_t a = addr - offset;
  int b = bytes;
  const char *nv = new_value;
  while (b > 0)
    {
      union
      {
	uintptr_t word;
	char bytes[sizeof (uintptr_t)];
      } value = { 0 };
      if (offset || b < sizeof (uintptr_t))
	{
	  errno = 0;
	  value.word = ptrace (PTRACE_PEEKDATA, tcb->tid, a);
	  if (errno)
	    {
	      debug (0, "Failed to read process %d's memory, "
		     "location %"PRIxPTR": %m",
		     tcb->tid, a);
	      return false;
	    }
	}

      int w = b;
      if (w > sizeof (uintptr_t) - offset)
	w = sizeof (uintptr_t) - offset;

      memcpy (&value.bytes[offset], nv, w);

      nv += w;
      offset = 0;
      b -= w;

      if (ptrace (PTRACE_POKEDATA, tcb->tid, a, value.word) < 0)
	{
	  debug (0, "Failed to write to process %d's memory, "
		 "location %"PRIxPTR": %m",
		 tcb->tid, a);
	  return false;
	}

      a += sizeof (uintptr_t);
    }

  do_debug (4)
    {
      GString *s = g_string_new ("");
      g_string_append_printf
	(s, "%d: Patched address 0x%"PRIxPTR" to contain:",
	 tcb->tid, addr);

      int i;
      for (i = 0; i < bytes; i ++)
	g_string_append_printf
	  (s, " 0x%02x", (int) (unsigned char) new_value[i]);
      debug (0, "%s", s->str);
      g_string_free (s, true);
    }

  return true;
}

/* The list of system calls that we want to intercept.  */
static long fixups[] =
  {
    __NR_open,
    __NR_close,
    __NR_openat,
    __NR_unlink,
    __NR_unlinkat,
    __NR_rmdir,
    __NR_rename,
    __NR_renameat,
  };

/* A list of instruction families, which we care about.  */
enum
  {
    ins_unknown = 0,
    ins_syscall,
    ins_syscall_errno_check,
    ins_syscall_num_load,
    ins_breakpoint,
    INS_COUNT
  };

static const char *
instruction_str (int ins)
{
  switch (ins)
    {
    case ins_syscall:
      return "syscall";
    case ins_syscall_errno_check:
      return "syscall errno check";
    case ins_syscall_num_load:
      return "syscall num load";
    case ins_breakpoint:
      return "breakpoint";
    default:
      return "unknown";
    }
}

/* A description of an instruction family.  */
struct instruction
{
  /* The length, in bytes.  */
  int len;
  union
  {
    uint64_t word64[INSTRUCTION_LEN_MAX / sizeof (uint64_t)];
    uint32_t word32[INSTRUCTION_LEN_MAX / sizeof (uint32_t)];
    char byte[INSTRUCTION_LEN_MAX];
  };
  /* A bit mask of the bytes to ignore.  */
  uint32_t skip;
};

#ifdef __arm__
static const struct instruction ins_syscall_num_load_bits[] =
  {
    /* mov	r7, #5	; 0x5 */
    { 4, { .byte = { /* syscall here.  */ 0x00, 0x70, 0xa0, 0xe3 } },
      .skip = (1 << 0) },
  };

static int syscall_num_load_horizon = 4;

static const struct instruction ins_syscall_bits[] =
  {
    /* svc */
    { 4, { .word32 = { 0xef000000 } } },
  };

static const struct instruction ins_syscall_errno_check_bits[] =
  {
    /* cmn	r0, #4096	; 0x1000 */
    { 4, { .word32 = { 0xe3700a01 } } },
  };

static int syscall_errno_check_horizon = 4;

static const struct instruction ins_breakpoint_bits =
  { 4, { .word32 = { 0xe7f001f0 } } };

#define INSTRUCTION_STEP (sizeof (uintptr_t))

#elif defined (__x86_64__)

static const struct instruction ins_syscall_num_load_bits[] =
  {
    /* mov    $0xXX,%eax */
    { 5, { .byte = { 0xb8, /* syscall num.  */ 0x00, 0x00, 0x00, 0x00 } },
      .skip = (1 << 1) | (1 << 2) },
    /* mov    $0xXXX,%ax */
    { 4, { .byte = { 0x66, 0xb8, /* syscall num.  */ 0x00, 0x00 } },
      .skip = (1 << 2) | (1 << 3) },
    /* mov $0xXX,%al */
    { 2, { .byte = { 0xb0, /* syscall num.  */ } }, .skip = 1 << 1 }
  };

static int syscall_num_load_horizon = 24;

static const struct instruction ins_syscall_bits[] =
  {
    { 4, { .word32 = { 0x3d48050f } } },
    { 4, { .word32 = { 0x8b48050f } } },
  };

static const struct instruction ins_syscall_errno_check_bits[] =
  {
    /* cmp    $0xfffffffffffff001,%rax */
    { 6, { .byte = { 0x48, 0x3d, 0x01, 0xf0, 0xff, 0xff } } },
    /* cmp    $0xfffffffffffff000,%rax */
    { 6, { .byte = { 0x48, 0x3d, 0x00, 0xf0, 0xff, 0xff } } },
    /* cmpb   $0x0,(%rax) */
    { 3, { .byte = { 0x80, 0x38, 0x00 } } },
  };

static int syscall_errno_check_horizon = 24;

static const struct instruction ins_breakpoint_bits =
  { 1, { .byte = { 0xCC } } };

#define INSTRUCTION_STEP 1
#endif

struct instruction_info
{
  /* Read in little endian order.  */
  uintptr_t skipped_value;
  int ins_len;
  char ins_bits[INSTRUCTION_LEN_MAX];
};

/* Return whether MEM points to an instruction from the instruction
   family INS.  If INFO is not NULL, fill in information about the
   instruction.  */
static bool
instruction_is (char *mem, int len,
		int ins, struct instruction_info *info)
{
  const struct instruction *i;
  int count;

  switch (ins)
    {
    case ins_syscall_num_load:
      i = ins_syscall_num_load_bits;
      count = sizeof (ins_syscall_num_load_bits)
	/ sizeof (ins_syscall_num_load_bits[0]);
      break;

    case ins_syscall:
      i = ins_syscall_bits;
      count = sizeof (ins_syscall_bits) / sizeof (ins_syscall_bits[0]);
      break;

    case ins_syscall_errno_check:
      i = ins_syscall_errno_check_bits;
      count = sizeof (ins_syscall_errno_check_bits)
	/ sizeof (ins_syscall_errno_check_bits[0]);
      break;

    case ins_breakpoint:
      i = &ins_breakpoint_bits;
      count = 1;
      break;

    default:
      debug (0, "Bad value (%d) for INS", ins);
      abort ();
    }

  int j, k;
  for (j = 0; j < count; j ++)
    {
      if (len < i[j].len)
	/* Instruction is longer than the data.  */
	continue;

      if (info)
	memset (info, 0, sizeof (*info));

      int skip_offset = 0;
      for (k = 0; k < i[j].len; k ++)
	if (((1 << k) & i[j].skip))
	  /* Skip this byte.  */
	  {
	    if (info)
	      {
		info->skipped_value |= (unsigned char) mem[k] << skip_offset;
		skip_offset += 8;
	      }
	  }
	else if (mem[k] != i[j].byte[k])
	  /* The byte does not match what's in memory.  */
	  break;

      if (k == i[j].len)
	/* Full match!  */
	{
	  if (info)
	    {
	      info->ins_len = i[j].len;
	      memcpy (info->ins_bits, mem, info->ins_len);
	    }

	  debug (4, "Found %s instruction (variant %d, len: %d)",
		 instruction_str (ins), j, i[j].len);

	  return true;
	}
    }
  return false;
}

static bool
thread_instruction_is (struct tcb *tcb, uintptr_t addr, int instruction)
{
  char ins[INSTRUCTION_LEN_MAX];

  char *end = tcb_mem_read (tcb, addr, ins, sizeof (ins), false);
  if (! end)
    {
      debug (0, "tcb_mem_read failed.");
      return ins_unknown; 
    }

  int ins_len = (uintptr_t) end - (uintptr_t) ins + 1;

  bool ret = instruction_is (ins, ins_len, instruction, NULL);
  if (! ret)
    {
      GString *s = g_string_new ("");
      g_string_append_printf
	(s, "Address %"PRIxPTR" does not contain a %s instruction:",
	 addr, instruction_str (instruction));

      int i;
      for (i = 0; i < ins_len; i ++)
	g_string_append_printf
	  (s, " 0x%02x", (int) (unsigned char) ins[i]);
      debug (0, "%s", s->str);
      g_string_free (s, true);
    }
  return ret;
}

/* Revert any fixups applied to a process.  */
static void
thread_revert_patches (struct tcb *tcb)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  int i;
  for (i = 0; i < LIBRARY_COUNT; i ++)
    if (tcb->pcb->lib_base[i])
      {
	uintptr_t base = tcb->pcb->lib_base[i];
	struct library_patch *lib = &library_patches[i];

	int bad = 0;
	int j;
	for (j = 0; j < lib->patch_count; j ++)
	  {
	    struct patch *p = &lib->patches[j];

	    uintptr_t addr = base + p->base_offset;

	    switch (thread_check (tcb, addr,
				  ins_breakpoint_bits.byte,
				  ins_breakpoint_bits.len))
	      {
	      default:
		bad ++;
		break;
	      case 1:
		do_debug (4)
		  {
		    GString *s = g_string_new ("");
		    g_string_append_printf
		      (s, "%d: Reverting patch at %"PRIxPTR" to contain:",
		       tcb->tid, addr);

		    int i;
		    for (i = 0; i < p->ins_len; i ++)
		      g_string_append_printf
			(s, " 0x%02x", (int) (unsigned char) p->ins[i]);
		    debug (0, "%s", s->str);
		    g_string_free (s, true);
		  }
		
		thread_mem_update (tcb, addr, p->ins, p->ins_len);
		break;
	      case -ESRCH:
		/* The process is dead.  */
		return;
	      }
	  }

	if (bad)
	  debug (0, DEBUG_BOLD ("Patched process %d missing "
				"%d of %d patches for %s."),
		 tcb->pcb->group_leader.tid, bad, lib->patch_count,
		 lib->filename);

	tcb->pcb->lib_base[i] = 0;
      }
}

static void
thread_apply_patches (struct tcb *tcb)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  bool fully_patched (void)
  {
    bool ret = true;
    int bad = 0;
    int total = 0;
    int i;
    for (i = 0; i < LIBRARY_COUNT; i ++)
      {
	uintptr_t base = tcb->pcb->lib_base[i];
	if (base)
	  {
#ifndef NDEBUG
	    struct library_patch *lib = &library_patches[i];

	    int j;
	    for (j = 0; j < lib->patch_count; j ++)
	      {
		struct patch *p = &lib->patches[j];

		uintptr_t addr = base + p->base_offset;
		if (! thread_instruction_is (tcb, addr, ins_breakpoint))
		  {
		    debug (0, "Bad patch at %"PRIxPTR" "
			   "in lib %s, offset %"PRIxPTR,
			   addr, lib->filename, p->base_offset);
		    bad ++;
		  }
		total ++;
	      }
#endif
	  }
	else
	  ret = false;
      }

    if (bad)
      debug (0, DEBUG_BOLD ("%d: %d of %d locations incorrectly patched"),
	     tcb->pcb->group_leader.tid, bad, total);
    return ret;
  }

  if (fully_patched ())
    {
      debug (4, "Process %d already fully patched.",
	     tcb->pcb->group_leader.tid);
      return;
    }

  void scan (struct library_patch *lib,
	     uintptr_t map_start, uintptr_t map_end)
  {
    assert (lib->patch_count == 0);
    assert (map_start < map_end);

    uintptr_t map_length = map_end - map_start;

    debug (4, DEBUG_BOLD ("Scanning %s: %"PRIxPTR"-%"PRIxPTR" "
			  "(0x%"PRIxPTR" bytes)"),
	   lib->filename, map_start, map_end, map_length);

    char *map = g_malloc (map_length);
    char *end = tcb_mem_read (tcb, map_start, map, map_length, false);
    if ((uintptr_t) end + 1 - (uintptr_t) map != map_length)
      debug (0, DEBUG_BOLD ("Reading library %s from process %d: "
			    "read %"PRIdPTR" bytes, expected %"PRIdPTR),
	     lib->filename, tcb->pcb->group_leader.tid,
	     (uintptr_t) end + 1 - (uintptr_t) map, map_length);
    if (! end)
      {
	debug (0, "tcb_mem_read failed.");
	return;
      }
    map_length = (uintptr_t) end - (uintptr_t) map + 1;

    /* We want to cache the real map length, not the truncated
       one.  */
    lib->size = map_length;


    GSList *patches = NULL;

    int syscall_count = 0;
    int sigs[syscall_num_load_horizon + 1][syscall_errno_check_horizon + 1][2];
    memset (sigs, 0, sizeof (sigs));

    uintptr_t off;
    for (off = INSTRUCTION_STEP; off < map_length; off += INSTRUCTION_STEP)
      {
	int mov = 0;
	int errno_check = 0;

	if (instruction_is (&map[off], map_length - off, ins_syscall, NULL))
	  {
	    syscall_count ++;

	    struct instruction_info mov_info;
	    int j;
	    for (j = 1; j <= syscall_num_load_horizon; j ++)
	      if (off > j * INSTRUCTION_STEP
		  && instruction_is (&map[off - j * INSTRUCTION_STEP],
				     map_length - (off - j * INSTRUCTION_STEP),
				     ins_syscall_num_load,
				     &mov_info))
		{
		  mov = j;
		  break;
		}
	    for (j = 1; j <= syscall_errno_check_horizon; j ++)
	      if (off + j * INSTRUCTION_STEP < map_length
		  && instruction_is (&map[off + j * INSTRUCTION_STEP],
				     map_length - (off + j * INSTRUCTION_STEP),
				     ins_syscall_errno_check,
				     NULL))
		{
		  errno_check = j;
		  break;
		}

	    sigs[mov][errno_check][0] ++;

	    if (mov == 0 || errno_check == 0)
	      debug (4, "Partial syscall signature match at 0x%"PRIxPTR" "
		     "mov: %d, errno check: %d",
		     off, mov, errno_check);

	    /* System call instruction.  Print the context.  */
	    debug (5, "Candidate syscall at %"PRIxPTR": "
		   "%08"PRIx32" %08"PRIx32" %08"PRIx32" "
		   "%08"PRIx32" *%08"PRIx32"* %08"PRIx32" "
		   "%08"PRIx32" %08"PRIx32" %08"PRIx32"%s%s",
		   off,
		   * (uint32_t *) &map[off - 4 * sizeof (uint32_t)],
		   * (uint32_t *) &map[off - 3 * sizeof (uint32_t)],
		   * (uint32_t *) &map[off - 2 * sizeof (uint32_t)],
		   * (uint32_t *) &map[off - 1 * sizeof (uint32_t)],
		   * (uint32_t *) &map[off],
		   * (uint32_t *) &map[off + 1 * sizeof (uint32_t)],
		   * (uint32_t *) &map[off + 2 * sizeof (uint32_t)],
		   * (uint32_t *) &map[off + 3 * sizeof (uint32_t)],
		   * (uint32_t *) &map[off + 4 * sizeof (uint32_t)],
		   mov ? " <<" : "", errno_check ? " >>" : "");

	    if (mov && errno_check)
	      /* Got a good system call signature.  */
	      {
		int j;
		for (j = 0; j < sizeof (fixups) / sizeof (fixups[0]); j ++)
		  if (mov_info.skipped_value == fixups[j])
		    {
		      sigs[mov][errno_check][1] ++;

		      struct patch *p = alloca (sizeof (*p) + mov_info.ins_len);

		      p->base_offset = (off - mov * INSTRUCTION_STEP);
		      p->syscall = fixups[j];
		      p->ins_len = mov_info.ins_len;
		      memcpy (p->ins, mov_info.ins_bits, mov_info.ins_len);

		      do_debug (0)
			{
			  GString *s = g_string_new ("");
			  g_string_append_printf
			    (s, "Patch for offset: %"PRIxPTR", syscall: %ld, "
			     "instruction (%d bytes):",
			     p->base_offset, p->syscall, p->ins_len);
			  int i;
			  for (i = 0; i < p->ins_len; i ++)
			    g_string_append_printf
			      (s, " 0x%02x", (int) (unsigned char) p->ins[i]);
			  debug (0, "%s", s->str);
			  g_string_free (s, true);
			}

		      patches = g_slist_prepend (patches, p);
		      lib->patch_count ++;
		      break;
		    }
	      }
	  }
      }

    debug (0, "%d system call sites in %s.", syscall_count, lib->filename);
    int i;
    for (i = 0; i < 2; i ++)
      {
	GString *s = g_string_new ("\n  errno check displacement\n");
	int j, k;
	g_string_append_printf (s, "   ");
	for (j = 0; j <= syscall_errno_check_horizon; j ++)
	  g_string_append_printf (s, "%4d", j);
	for (j = 0; j <= syscall_num_load_horizon; j ++)
	  {
	    g_string_append_printf (s, "\n%2d ", j);
	    for (k = 0; k <= syscall_errno_check_horizon; k ++)
	      g_string_append_printf (s, "%4d", sigs[j][k][i]);
	  }
	debug (0, "%s", s->str);
	g_string_free (s, true);
      }

    if (lib->patch_count == 0)
      lib->patch_count = -1;
    debug (0, "%s: %d patches.", lib->filename, lib->patch_count);

    g_free (map);

    lib->patches = g_malloc (sizeof (struct patch) * lib->patch_count);
    for (i = 0; i < lib->patch_count; i ++)
      {
	assert (patches);
	struct patch *p = patches->data;
	lib->patches[i] = *p;

	patches = g_slist_delete_link (patches, patches);
      }
    assert (! patches);
  }

  /* 0 => Library not found.  1 => Good library found, MAP_START and
     MAP_END valid.  -1 => Library found, but version mismatch or some
     other error.  */
  int grep_maps (const char *maps, struct library_patch *lib,
		 uintptr_t *map_start, uintptr_t *map_end)
  {
    int filename_len = strlen (lib->filename);

    int ret = 0;
    const char *filename_loc = maps;
    while ((filename_loc = strstr (filename_loc, lib->filename)))
      {
	/* We have, e.g., /lib/libc-, make sure it is something like
	   /lib/libc-2.11.so and not /usr/lib/libc-other.so  */
	if (filename_loc == maps || filename_loc[-1] != ' ')
	  continue;
	int suffix = strspn (filename_loc + filename_len,
			     "1234567890-.so");
	if (filename_loc[filename_len + suffix] != '\n')
	  continue;

	/* Find the start of the line.  */
	const char *line
	  = memrchr (maps, '\n', (uintptr_t) filename_loc - (uintptr_t) maps);
	if (line)
	  line ++;
	else
	  line = maps;

	debug (4, "Considering '%.*s'",
	       (int) ((uintptr_t) filename_loc - (uintptr_t) line
		      + filename_len + suffix),
	       line);

	const char *p = line;

	/* Advance by one so that the next strstr (if any) doesn't
	   return the same location.  */
	filename_loc ++;

	char *tailp = NULL;
	errno = 0;
	*map_start = strtoull (p, &tailp, 16);
	if (errno)
	  {
	    debug (0, "Overflow reading number: %.12s", p);
	    return -1;
	  }
	debug (4, "map start: %"PRIxPTR, *map_start);
	p = tailp;
	if (*p != '-')
	  {
	    debug (0, "Expected '-'.  Have '%c'", *p);
	    return -1;
	  }
	p ++;

	errno = 0;
	*map_end = strtoull (p, &tailp, 16);
	if (errno)
	  {
	    debug (0, "Overflow reading number: %.12s", p);
	    return -1;
	  }
	debug (4, "map end: %"PRIxPTR, *map_end);
	p = tailp;

	const char *perms = " r-xp ";
	if (strncmp (p, perms, strlen (perms)) != 0)
	  /* Right library, wrong section.  Skip it.  */
	  {
	    debug (5, "Expected '%s'.  Have '%.6s'", perms, p);
	    continue;
	  }

	p += strlen (perms);

	errno = 0;
	uintptr_t file_offset = strtoull (p, &tailp, 16);
	if (errno)
	  {
	    debug (0, "Overflow reading number: %.12s", p);
	    return -1;
	  }
	debug (4, "file offset: %"PRIxPTR, file_offset);
	p = tailp;
	if (*p != ' ')
	  {
	    debug (0, "Expected ' '.  Have '%.12s'", p);
	    return -1;
	  }

	if (lib->size)
	  /* (Try to) make sure it is the same library.  */
	  {
	    if (*map_end - *map_start != lib->size)
	      /* Wrong library.  */
	      {
		debug (0, "%d: Found library %s at 0x%"PRIxPTR"-0x%"PRIxPTR", "
		       "but wrong size "
		       "(expected 0x%"PRIxPTR", got 0x%"PRIxPTR").",
		       tcb->tid, lib->filename, *map_start, *map_end,
		       lib->size, *map_end - *map_start);
		ret = -1;
		continue;
	      }
	  }

	debug (0, DEBUG_BOLD ("%d: Found library %s at "
			      "0x%"PRIxPTR"-0x%"PRIxPTR" (0x%"PRIxPTR")"),
	       tcb->tid, lib->filename,
	       *map_start, *map_end, *map_end - *map_start);

	return 1;
      }
    return ret;
  }

  /* Figure our where the libraries are mapped.  */
  char filename[32];
  gchar *maps = NULL;
  gsize length = 0;
  GError *error = NULL;
  snprintf (filename, sizeof (filename),
	    "/proc/%d/maps", tcb->tid);
  if (! g_file_get_contents (filename, &maps, &length, &error))
    {
      debug (0, "Error reading %s: %s; can't trace non-existent process",
	     filename, error->message);
      g_error_free (error);
      error = NULL;
      return;
    }

  debug (4, "%s: %s", filename, maps);

  bool patch[LIBRARY_COUNT];
  int i;
  for (i = 0; i < LIBRARY_COUNT; i ++)
    {
      patch[i] = false;
      if (! tcb->pcb->lib_base[i])
	{
	  uintptr_t map_addr, map_end;
	  switch (grep_maps (maps, &library_patches[i],
			     &map_addr, &map_end))
	    {
	    case 1:
	      if (! library_patches[i].patch_count)
		/* We have not yet generated the patch list.  */
		scan (&library_patches[i], map_addr, map_end);
	      tcb->pcb->lib_base[i] = map_addr;
	      patch[i] = true;
	      break;

	    case -1:
	      /* Don't patch anything.  */
	      {
		int j;
		for (j = 0; j < i; j ++)
		  if (patch[j])
		    tcb->pcb->lib_base[j] = 0;
		thread_revert_patches (tcb);
		g_free (maps);
		return;
	      }
	    }
	}
    }

  g_free (maps);

  /* Patch the current process.  */
  int bad = 0;
  int total = 0;
  for (i = 0; i < LIBRARY_COUNT; i ++)
    if (patch[i])
      {
	int j;
	for (j = 0; j < library_patches[i].patch_count; j ++)
	  {
	    struct patch *p = &library_patches[i].patches[j];
	    if (thread_check (tcb, tcb->pcb->lib_base[i] + p->base_offset,
			      p->ins, p->ins_len) != 1)
	      bad ++;
	  }

	total += j;
      }

  if (bad)
    {
      debug (0, DEBUG_BOLD ("%d of %d locations contain unexpected values.  "
			    "Not patching."),
	     bad, total);
      return;
    }

  int count = 0;
  for (i = 0; i < LIBRARY_COUNT; i ++)
    if (patch[i])
      {
	int j;
	for (j = 0; j < library_patches[i].patch_count; j ++)
	  {
	    struct patch *p = &library_patches[i].patches[j];
	    if (thread_mem_update (tcb, tcb->pcb->lib_base[i] + p->base_offset,
				   ins_breakpoint_bits.byte,
				   ins_breakpoint_bits.len))
	      count ++;
	  }
      }

  debug (4, "Patched %d %s;%s;%s: applied %d of %d patches",
	 tcb->pcb->group_leader.tid,
	 tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1,
	 count, total);

#ifndef NDEBUG
  fully_patched ();
#endif
}

#ifdef __x86_64__
# define REGS_STRUCT struct user_regs_struct
# define REGS_IP rip
# define REGS_SYSCALL rax
#elif __arm__
# define REGS_STRUCT struct pt_regs
# define REGS_IP ARM_pc
# define REGS_SYSCALL ARM_r7
#endif

/* The thread hit a break point.  If it is a breakpoint we set, fix up
   the thread's register state and return true.  Otherwise, return
   false.  */
static bool
thread_fixup_and_advance (struct tcb *tcb, REGS_STRUCT *regs)
{
  if (! pcb_patched (tcb->pcb))
    return false;

  uintptr_t IP = regs->REGS_IP;
  bool fixed = false;
  int i;
  for (i = 0; i < LIBRARY_COUNT; i ++)
    if (tcb->pcb->lib_base[i]
	&& tcb->pcb->lib_base[i] <= IP
	&& IP < tcb->pcb->lib_base[i] + library_patches[i].size)
      /* The address is coverd by this library.  */
      {
	struct library_patch *lib = &library_patches[i];
	debug (4, "Thread %d: IP %"PRIxPTR" covered by library %s "
	       "(=> offset: %"PRIxPTR")",
	       tcb->tid, IP, lib->filename, IP - tcb->pcb->lib_base[i]);

	int j;
	for (j = 0; j < lib->patch_count; j ++)
	  {
	    struct patch *p = &lib->patches[j];

	    uintptr_t addr = tcb->pcb->lib_base[i] + p->base_offset;
	    if (addr == IP || addr + ins_breakpoint_bits.len == IP)
	      {
		regs->REGS_IP = addr + p->ins_len;
		regs->REGS_SYSCALL = (uintptr_t) p->syscall;

		if (ptrace (PTRACE_SETREGS, tcb->tid, 0, (void *) regs) < 0)
		  debug (0, "Failed to update thread %d's register set: %m",
			 tcb->tid);
		else
		  {
		    fixed = true;
		    debug (4, "Thread %d: Fixed up thread "
			   "(%s, syscall %s (%ld)).",
			   tcb->tid, lib->filename,
			   syscall_str (p->syscall), p->syscall);
		  }

		break;
	      }
	  }

	/* As libraries don't overlap, we're done.  */
	break;
      }

  if (i == LIBRARY_COUNT)
    debug (4, "Thread %d: IP %"PRIxPTR" not covered by any library.",
	   (int) tcb->tid, (uintptr_t) regs->REGS_IP);

  if (! fixed)
    debug (4, "Thread %d: IP: %"PRIxPTR".  No fix up applied.",
	   (int) tcb->tid, (uintptr_t) regs->REGS_IP);

  return fixed;
}

/* Stop tracing a process.  Don't release its TCB.  */
static void
thread_detach (struct tcb *tcb)
{
  debug (4, "Detaching %d", (int) tcb->tid);
  if (ptrace (PTRACE_DETACH, tcb->tid, 0, SIGCONT) < 0)
    debug (0, "Detach from %d failed: %m", tcb->tid);
}

/* Free a TCB data structure.  The thread must already be detached,
   i.e., it terminated or we called ptrace (PTRACE_DETACH, tcb->tid).
   Remove it from the TCB hash, unlink it from its pcb and release the
   memory.  */
static void
thread_untrace (struct tcb *tcb, bool need_detach)
{
  debug (3, "thread_untrace (%d %s;%s;%s)",
	 tcb->tid, tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1);

  assert (pthread_equal (pthread_self (), process_monitor_tid));

  assert (g_slist_find (tcb->pcb->tcbs, tcb));
  tcb->pcb->tcbs = g_slist_remove (tcb->pcb->tcbs, tcb);

  if (! g_hash_table_remove (tcbs, (gpointer) (uintptr_t) tcb->tid))
    {
      debug (0, "Failed to remove tcb for %d from TCBS hash table?!?",
	     (int) tcb->tid);
      assert (0 == 1);
    }

  if (tcb->suspended)
    {
      assert (g_slist_find (suspended_tcbs, tcb));
      suspended_tcbs = g_slist_remove (suspended_tcbs, tcb);
    }

  g_free (tcb->saved_src);
  g_free (tcb->saved_stat);

  if (tcb->memfd != -1)
    {
      close (tcb->memfd);
      memfd_count --;
    }

  /* After calling pcb_free, TCB may be freed.  Copy some data.  */
  pid_t tid = tcb->tid;
  pid_t pid = tcb->pcb->group_leader.tid;
  bool need_free = (tcb != &tcb->pcb->group_leader);

  tcb_count --;

  if (tcb->pcb->tcbs == NULL)
    /* We were the last thread in the process.  Free the PCB.  */
    {
      if (need_detach)
	/* We need to revert any patches we applied: the thread may
	   not have actually exited, but been detached by the user.
	   We have to do this before we PTRACE_DETACH from the
	   thread.  */
	{
	  debug (0, "Reverting patches on process %d (last thread %d, quit)",
		 pid, tid);
	  thread_revert_patches (tcb);
	}
      pcb_free (tcb->pcb);
    }

  /* Don't touch TCB!  pcb_free may have freed it!  */
  tcb = NULL;

  if (need_detach)
    {
      debug (4, "Detaching %d", (int) tid);
      if (ptrace (PTRACE_DETACH, tid, 0, SIGCONT) < 0)
	debug (0, "Detach from %d failed: %m", tid);
    }

  if (need_free)
    g_free (tcb);

  debug (4, "%d processes still being traced (%d threads)",
	 pcb_count, tcb_count);
}

/* Start tracing thread TID.  PARENT is the process that stared the
   thread (if not known, specify NULL).  ALREADY_PTRACING indicates
   whether the thread is being ptraced.  If the thread is not already
   being ptraced, we attach to it.  */
static struct tcb *
thread_trace (pid_t tid, struct pcb *parent, bool already_ptracing)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  debug (3, "thread_trace (tid: %d, parent: %d, %s ptracing)",
	 (int) tid, parent ? parent->group_leader.tid : 0,
	 already_ptracing ? "already" : "not yet");

  struct tcb *tcb = g_hash_table_lookup (tcbs, (gpointer) (uintptr_t) tid);
  if (tcb)
    /* It's already being traced.  This happens in two cases: when we
       see the SIGSTOP before the PTRACE_EVENT_CLONE event; and, when
       the user explicitly traces a process that is already being
       traced.  */
    {
      assert (tid == tcb->tid);

      if (tid == tcb->pcb->group_leader.tid)
	/* TID is the process group leader.  PARENT is the process's
	   (as opposed to the thread's) parent.  */
	{
	  assert (tcb == &tcb->pcb->group_leader);
	  /* A process's parent can't be itself...  */
	  assert (parent != tcb->pcb);
	  if (parent)
	    pcb_parent_set (tcb->pcb, parent);
	}

      return tcb;
    }

  pid_t pgl = tid_to_process_group_leader (tid);
  if (! pgl)
    /* Failed to get the process group leader.  The thread no longer
       exists.  */
    return NULL;

  struct pcb *pcb = g_hash_table_lookup (pcbs, (gpointer) (uintptr_t) pgl);
  if (! pcb)
    /* This is the first thread in an as-yet unknown process.  */
    {
      pcb = g_malloc0 (sizeof (struct pcb));

      pcb->group_leader.tid = pgl;

      g_hash_table_insert (pcbs, (gpointer) (uintptr_t) pgl, pcb);
      pcb_count ++;

      pcb_read_exe (pcb);

      int i;
      for (i = 0; i < LIBRARY_COUNT; i ++)
	pcb->lib_fd[i] = -1;

      debug (4, "%d processes being traced (%d threads)",
	     pcb_count, tcb_count);

      if (tid != pgl)
	/* We've added a thread before we've added the group leader.
	   (It can happen depending on how thread creation events are
	   ordered.)  Add the group leader now.  Then add TID.  */
	thread_trace (pgl, NULL, false);

      /* We'll figure out the process's parent when the group leader
	 gets explicitly added.  */
    }


  if (tid == pgl)
    {
      tcb = &pcb->group_leader;
      /* TID is the process group leader.  PARENT is the process's (as
	 opposed to the thread's) parent.  */
      assert (pcb != parent);
      if (parent)
	pcb_parent_set (pcb, parent);
    }
  else
    tcb = g_malloc0 (sizeof (*tcb));

  tcb->tid = tid;
  tcb->pcb = pcb;
  tcb->current_syscall = -1;
  tcb->memfd = -1;
  g_hash_table_insert (tcbs, (gpointer) (uintptr_t) tid, tcb);
  pcb->tcbs = g_slist_prepend (pcb->tcbs, tcb);

  tcb_count ++;

  /* NB: After tracing a process, we first receive two SIGSTOPs
     (signal 0x13) for that process.  Subsequently, we receive
     SIGTRAPs on system calls.  */
  if (! already_ptracing)
    {
      if (ptrace (PTRACE_ATTACH, tid) == -1)
	{
	  debug (0, "Error attaching to %d: %m", tid);
	  thread_untrace (tcb, false);
	  return NULL;
	}
    }

  debug (0, "Now tracing thread %d (%s;%s;%s).  Process: %d",
	 tid, pcb->exe, pcb->arg0, pcb->arg1, pcb->group_leader.tid);

  debug (4, "%d processes being traced (%d threads)",
	 pcb_count, tcb_count);

  return tcb;
}

/* Start tracing a process.  Return the corresponding PCB for the
   thread group leader.  */
static struct pcb *
process_trace (pid_t pid)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  struct tcb *tcb = thread_trace (pid, NULL, false);
  if (! tcb)
    {
      callback_enqueue (tcb, WC_PROCESS_TRACING_CB, NULL, NULL, false, NULL);
      return NULL;
    }

  /* The user is explicitly adding this process.  Make it a top-level
     process.  */
  tcb->pcb->top_level = true;

  callback_enqueue (tcb, WC_PROCESS_TRACING_CB, NULL, NULL, true, NULL);

  return tcb->pcb;
}

/* Stop tracing a process and any children.  The corresponding PCB(s)
   will be freed once all of the associated threads have exited.  */
static void
process_untrace (pid_t pid)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));

  struct pcb *pcb = g_hash_table_lookup (pcbs, (gpointer) (uintptr_t) pid);
  if (! pcb)
    /* A user can call untrace on a PID more than once.  */
    {
      debug (0, "Can't untrace %d: not being traced.", pid);
      return;
    }

  if (! pcb->top_level)
    {
      assert (pcb->parent);
      debug (0, "Bad untrace: %d never explicitly traced.", pid);
      return;
    }

  if (pcb->group_leader.stop_tracing)
    {
      debug (0, "Already untracing %d.", pid);
      return;
    }

  if (pcb->parent)
    /* Don't actually detach.  An ancestor is still traced.  */
    {
      pcb->top_level = false;
      return;
    }

  debug (4, "Will detach from %d (and children) at next opportunity.", pid);

  GSList *dofree = NULL;
  void stop (struct pcb *pcb)
  {
    GSList *t;
    for (t = pcb->tcbs; t; t = t->next)
      {
	struct tcb *tcb = t->data;
	if (tcb->suspended <= 0 && tkill (tcb->tid, SIGSTOP) < 0)
	  {
	    debug (0, "tkill (%d, SIGSTOP): %m", tcb->tid);
	    dofree = g_slist_prepend (dofree, tcb);
	  }
	tcb->stop_tracing = true;
      }

    GSList *l;
    for (l = pcb->children; l; l = l->next)
      {
	struct pcb *child = l->data;
	assert (child->parent == pcb);
	assert (! child->group_leader.stop_tracing);
	if (! child->top_level)
	  stop (child);
      }
  }
  stop (pcb);

  while (dofree)
    {
      struct tcb *tcb = dofree->data;
      thread_untrace (tcb, false);
      dofree = g_slist_delete_link (dofree, dofree);
    }
}

static uint64_t quit;

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

static pid_t signal_process_pid;

static void
process_monitor_signaler_init (void)
{
  assert (pthread_equal (pthread_self (), process_monitor_tid));
  assert (! signal_process_pid);

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
	sleep (INT_MAX);
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
  debug (4, "Queuing process monitor event.");
  if (! process_monitor_commands->next)
    /* This is the only event.  */
    {
      debug (4, "Only event.  Signalling signal process.");
      if (tkill (signal_process_pid, SIGUSR2) < 0)
	debug (0, "killing signalling signal process (%d): %m",
	       signal_process_pid);
    }
  
  pthread_mutex_unlock (&process_monitor_commands_lock);
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
  debug (0, DEBUG_BOLD ("Suspending thread %d (process %d: %s;%s;%s): "
			"%d callbacks per second"),
	 tcb->tid, tcb->pcb->group_leader.tid,
	 tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1,
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

  struct tcb *tcb = NULL;
  while (! (quit && tcb_count == 0))
    {
      if (quit && now () - quit > 5000)
	/* If we don't manage to exit in about 5 seconds, something is
	   likely wrong.  Increase the debugging output.  */
	{
	  quit = now ();
	  output_debug = 5;

	  debug (0, "Need to detach from:");
	  void iter (gpointer key, gpointer value, gpointer user_data)
	  {
	    pid_t tid = (int) (uintptr_t) key;
	    struct tcb *tcb = value;
	    debug (1, "%d: %s;%s;%s",
		   tid, tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1);
	    assert (tid == tcb->tid);

	    if (tcb->suspended > 0)
	      /* Already suspended.  */
	      thread_untrace (tcb, false);
	    else if (tkill (tid, SIGSTOP) < 0)
	      /* Failed to send it SIGSTOP.  Assume it is
		 dead.  */
	      {
		debug (0, "tkill (%d, SIGSTOP): %m", tid);
		thread_untrace (tcb, false);
	      }
	  }
	  g_hash_table_foreach (tcbs, iter, NULL);
	}

      /* SIGNO is the signal the child received and the signal to be
	 propagated to the child (if any).  */
      int signo = 0;
      /* How to resume the process.  */
      int ptrace_op = PTRACE_CONT;

      /* Is there a race between checking for new commands and calling
	 waitpid?  No.  Consider the case that the process monitor
	 checks for new commands, finds none, drops the lock and then
	 is descheduled.  At this point, the user adds a new thread to
	 trace.  As the lock is free, the command is added to the
	 queued command list.  The process monitor now calls waitpid.
	 Because adding the command also tickled the wait process, we
	 will wake up immediately.  */
      int status = 0;
      pid_t tid = waitpid (-1, &status, __WALL|__WCLONE);
      if (tid == signal_process_pid || process_monitor_commands)
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
	      c = NULL;

	      switch (command)
		{
		case PROCESS_MONITOR_QUIT:
		  /* Quit.  This is easier said than done.  We want to
		     exit gracefully.  That means detaching from all
		     threads.  But we can only detach from a thread
		     when we've captured it.  Is there a way to force
		     the thread to do something so we can capture it
		     or do we have to wait for a convenient moment?
		     IT APPEARS that we can send a thread a SIGSTOP to
		     (reliably) wake it up.  */
		  debug (1, "Quitting.  Need to detach from:");

		  void iter (gpointer key, gpointer value, gpointer user_data)
		  {
		    pid_t tid = (int) (uintptr_t) key;
		    struct tcb *tcb = value;
		    debug (1, "%d: %s;%s;%s",
			   tid, tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1);
		    assert (tid == tcb->tid);

		    if (tcb->suspended > 0)
		      /* Already suspended.  */
		      thread_untrace (tcb, false);
		    else if (tkill (tid, SIGSTOP) < 0)
		      /* Failed to send it SIGSTOP.  Assume it is
			 dead.  */
		      {
			debug (0, "tkill (%d, SIGSTOP): %m", tid);
			thread_untrace (tcb, false);
		      }
		  }
		  g_hash_table_foreach (tcbs, iter, NULL);
		  tcb = NULL;

		  quit = now ();
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
	  if (tid == signal_process_pid)
	    goto out;
	}

      int event = status >> 16;

      do_debug (4)
	{
	  debug (0, "Signal from %d: status: %x", tid, status);
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
      if (! (tcb && tcb->tid == tid))
	{
	  tcb = g_hash_table_lookup (tcbs, (gpointer) (uintptr_t) tid);
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
		/* It's got the ptrace monitor bit set.  It was
		   started by some thread that we are tracing.  We'll
		   get ptrace event message soon.  */
		{
		  tcb = thread_trace (tid, NULL, true);
		  tcb->trace_options = 1;
		}
	      else
		debug (3, "Got signal for %d, but not monitoring it!", tid);

	      if (! tcb)
		goto out;
	    }
	}

      /* If the process has been fixed up, then let it run.
	 Otherwise, default to stopping it at the next signal.  */
      ptrace_op = pcb_patched (tcb->pcb) ? PTRACE_CONT : PTRACE_SYSCALL;

      /* See if the child exited.  */
      if (WIFEXITED (status))
	{
	  debug (4, "%d exited: %d.", (int) tid, WEXITSTATUS (status));
	  thread_untrace (tcb, false);
	  tcb = NULL;
	  continue;
	}

      if (WIFSIGNALED (status))
	{
	  debug (4, "%d exited due to signal: %s.",
		 (int) tid, strsignal (WTERMSIG (status)));
	  thread_untrace (tcb, false);
	  tcb = NULL;
	  continue;
	}

      /* The child is not dead.  Process the event.  */

      /* Resource accounting.  */
      load_increment (tcb);

      if (! WIFSTOPPED (status))
	/* Ignore.  Not stopped.  */
	{
	  debug (4, "%d: Not stopped.", (int) tid);
	  goto out;
	}

      signo = WSTOPSIG (status);

      if (quit || tcb->stop_tracing)
	/* We are trying to exit or we want to detach from this
	   process.  */
	{
	  debug (4, "Detaching %d", (int) tid);
	  /* Revert any patches before resuming the first thread in
	     the process.  */
	  thread_revert_patches (tcb);
	  thread_untrace (tcb, true);
	  tcb = NULL;
	  continue;
	}

      if (tcb->suspended < 0)
	/* Suspend pending.  Do it now.  */
	{
	  debug (4, "Detaching %d", (int) tid);
	  thread_detach (tcb);
	  tcb->suspended = -tcb->suspended;
	  continue;
	}

      void open_fds_iterate (int op)
      {
	assert (op == WC_PROCESS_CLOSE_CB || op == WC_PROCESS_OPEN_CB);

	char filename[32];
	snprintf (filename, sizeof (filename),
		  "/proc/%d/fd", tid);
	GError *error = NULL;
	GDir *d = g_dir_open (filename, 0, &error);
	if (! d)
	  {
	    debug (0, "Unable to open %s: %s",
		   filename, error->message);
	    g_free (error);
	    error = NULL;
	    return;
	  }

	const char *e;
	while ((e = g_dir_read_name (d)))
	  {
	    char buffer[1024];
	    snprintf (buffer, sizeof (buffer), "/proc/%d/fd/%s",
		      tid, e);
	    int ret = readlink (buffer, buffer, sizeof (buffer) - 1);
	    if (ret < 0)
	      {
		debug (0, "%d: Failed to read %s: %m", tid, buffer);
		return;
	      }

	    buffer[ret] = 0;

	    debug (4, TID_FMT": Open at %s: %s -> %s",
		   TID_PRINTF (tcb),
		   op == WC_PROCESS_CLOSE_CB ? "exit" : "attach", e, buffer);

	    if (process_monitor_filename_whitelisted (buffer))
	      {
		struct stat s;
		if (stat (buffer, &s) < 0)
		  {
		    debug (4, "Failed to stat %s: %m", buffer);
		    memset (&s, 0, sizeof (s));
		  }

		callback_enqueue (tcb, op,
				  buffer, NULL, 0, &s);
	      }
	  }
	g_dir_close (d);
      }

      if (tcb->trace_options == 0 && signo == SIGSTOP)
	/* This is a running thread that we have manually attached to.
	   The thread is now running.  */
	{
	  if (ptrace (PTRACE_SETOPTIONS, tid, 0,
		      (PTRACE_O_TRACESYSGOOD|PTRACE_O_TRACECLONE
		       |PTRACE_O_TRACEFORK|PTRACE_O_TRACEEXEC
		       |PTRACE_O_TRACEEXIT)) == -1)
	    {
	      debug (0, "Failed to set trace options on thread %d: %m", tid);
	      thread_untrace (tcb, true);
	      tcb = NULL;
	      continue;
	    }
	  else
	    tcb->trace_options = 1;

	  if (! tcb->pcb->scanned_siblings)
	    /* Find sibling threads.  */
	    {
	      tcb->pcb->scanned_siblings = true;

	      char filename[32];
	      snprintf (filename, sizeof (filename),
			"/proc/%d/task", tid);
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
			  int tid2 = atoi (e);
			  if (tid2)
			    {
			      struct tcb *tcb2 = g_hash_table_lookup
				(tcbs, (gpointer) (uintptr_t) tid2);
			      if (! tcb2)
				/* We are not monitoring it yet.  */
				{
				  if ((tcb2 = thread_trace (tid2, tcb->pcb,
							    false)))
				    added_one = true;
				}
			      else if (tid2 != tid)
				debug (4, "Already monitoring %d", tid2);
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

	  thread_apply_patches (tcb);

	  open_fds_iterate (WC_PROCESS_OPEN_CB);

	  goto out;
	}

      /* We are interested in system calls and events, not signals.  A
	 system call is indicated by setting the signal to
	 0x80|SIGTRAP.  If this is not the case, just forward the
	 signal to the process.  */
      if (! ((signo == (0x80 | SIGTRAP) || event || signo == SIGTRAP)))
	/* Ignore.  Not our signal.  */
	{
	  debug (4, "%d: ignoring and forwarding signal '%s' (%d) ",
		 tid, strsignal (signo), signo);
	  goto out;
	}

      /* When we resume the process, forward it the signal.  */
      signo = 0;

      if (event)
	{
	  unsigned long msg = -1;
	  if (ptrace (PTRACE_GETEVENTMSG, tid, 0, (uintptr_t) &msg) < 0)
	    debug (0, "PTRACE_GETEVENTMSG(%d): %m", tid);

	  switch (event)
	    {
	    case PTRACE_EVENT_EXEC:
	      /* XXX: WTF?  If we don't catch exec events, ptrace
		 options are cleared on exec, e.g., SIGTRAP is sent
		 without the high-bit set.  If we do set it, the
		 options are inherited.  */
	      debug (4, "%d: exec'd", tid);
	      pcb_read_exe (tcb->pcb);

	      /* It has a new memory image.  We need to fix it up.  */
	      memset (&tcb->pcb->lib_base, 0, sizeof (tcb->pcb->lib_base));
	      ptrace_op = PTRACE_SYSCALL;

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
		    struct tcb *tcb2 = thread_trace ((pid_t) child, tcb->pcb,
						     true);
		    /* If it is the same process, we already scanned
		       for siblings.  If it is a new process, it must
		       be the first (and only) thread, because it has
		       not yet had a chance to run.  */
		    tcb2->pcb->scanned_siblings = true;
		    /* It inherits the options set on its parent.  */
		    tcb2->trace_options = tcb->trace_options;

		    int i;
		    for (i = 0; i < LIBRARY_COUNT; i ++)
		      /* It has the same memory image.  If the parent
			 is fixed up, so is the child.  */
		      {
			if (event != PTRACE_EVENT_CLONE)
			  tcb2->pcb->lib_base[i] = tcb->pcb->lib_base[i];

			/* The same fds are open.  */
			tcb2->pcb->lib_fd[i] = tcb->pcb->lib_fd[i];
		      }
		  }

		goto out;
	      }

	    case PTRACE_EVENT_EXIT:
	      assert (tcb->pcb->tcbs);
	      if (! tcb->pcb->tcbs->next)
		/* The process is about to exit.  For each open file
		   descriptor, emit a close event.  */
		open_fds_iterate (WC_PROCESS_CLOSE_CB);
	      goto out;

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
# define REGS_FMT "rip: %"PRIxPTR"; rsp: %"PRIxPTR"; rax (ret): %"PRIxPTR"; " \
	"rbx: %"PRIxPTR"; rcx: %"PRIxPTR"; rdx (3): %"PRIxPTR"; "	\
	"rdi (0): %"PRIxPTR"; rsi (1): %"PRIxPTR"; r8 (5): %"PRIxPTR"; " \
	"r9 (6): %"PRIxPTR"; r10 (4): %"PRIxPTR"; r11: %"PRIxPTR"; "	\
	"r12: %"PRIxPTR"; r13: %"PRIxPTR"; r14: %"PRIxPTR"; "		\
	"r15: %"PRIxPTR"; orig rax (syscall): %"PRIxPTR
# define REGS_PRINTF(regs) (regs)->rip, (regs)->rsp, (regs)->rax,	\
	(regs)->rbx, (regs)->rcx, (regs)->rdx,				\
	(regs)->rdi, (regs)->rsi, (regs)->r8,				\
	(regs)->r9, (regs)->r10, (regs)->r11,				\
	(regs)->r12, (regs)->r13, (regs)->r14,				\
	(regs)->r15, (regs)->orig_rax
#elif __arm__
# define REGS_FMT "pc: %lx; sp: %lx; r0 (ret): %lx; " \
	"r1 (2): %lx; r2 (3): %lx; r3 (4): %lx; "	\
	"r4 (5): %lx; r5 (6): %lx; r6: %lx; "	\
	"r7: %lx; r8: %lx; r9: %lx; "	\
	"r10: %lx; orig r0 (1): %lx; ip: %lx"
# define REGS_PRINTF(regs) (regs)->ARM_pc, (regs)->ARM_sp, (regs)->ARM_r0, \
	(regs)->ARM_r0, (regs)->ARM_r1, (regs)->ARM_r2,			\
	(regs)->ARM_r3, (regs)->ARM_r4, (regs)->ARM_r5, \
	(regs)->ARM_r7, (regs)->ARM_r8, (regs)->ARM_r9, \
	(regs)->ARM_r10, (regs)->ARM_ORIG_r0, (regs)->ARM_ip

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
#else
# error Not ported to your architecture.
#endif
      REGS_STRUCT regs;
      memset (&regs, 0, sizeof (regs));

      if (ptrace (PTRACE_GETREGS, tid, 0, &regs) < 0)
	{
	  debug (0, "%d: Failed to get thread's registers: %m", (int) tid);
	  goto out;
	}

      ptrace_op = PTRACE_SYSCALL;

      if (WSTOPSIG (status) == SIGTRAP)
	/* See if we inserted a break point.  If so, try to advance
	   the thread.  If that works, just continue the thread.  It
	   will be back in a moment...  */
	if (thread_fixup_and_advance (tcb, &regs))
	  goto out;

      bool syscall_entry = tcb->current_syscall == -1;
#ifdef __x86_64__
      /* On x86-64, RAX is set to -ENOSYS on system call entry.  How
	 do we distinguish this from a system call that returns
	 ENOSYS?  */
      syscall_entry = regs.rax == -ENOSYS;
#elif defined(__arm__)
      /* ip is set to 0 on system call entry, 1 on exit.  */
      syscall_entry = regs.ARM_ip == 0;
#else
# error syscall_entry handling fragile.
#endif
      long syscall;
      if (syscall_entry)
	syscall = tcb->current_syscall = SYSCALL;
      else
	{
	  /* It would be nice to have:

	       assert (syscall == pcb->current_syscall)

	     Unfortunately, at least rt_sigreturn on x86-64 clobbers
	     the syscall number of syscall exit!  */
	  if (SYSCALL != tcb->current_syscall)
	    {
	      debug (4, "%d: warning syscall %ld entry "
		     "followed by syscall %ld!?!",
		     (int) tid, tcb->current_syscall, (long) SYSCALL);
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
		debug (0, "%d: Failed to read %s: %m", pid, buffer);
		return false;
	      }

	    buffer[ret] = 0;
	    return true;
	  }
	else
	  return false;
      }

      debug (4, "%d: %s (%ld) %s",
	     (int) tid, syscall_str (syscall), syscall,
	     syscall_entry ? "entry" : "exit");
      debug (5, REGS_FMT, REGS_PRINTF (&regs));

      switch (syscall)
	{
	default:
	  break;

	case __NR_open:
	case __NR_openat:
	  {
	    if (syscall_entry)
	      break;

	    /* We only care about the syscall exit.  */

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
		char *end = tcb_mem_read (tcb, ARG1, buffer,
					  sizeof (buffer) - 1, true);
		if (end)
		  /* Ensure the string is NUL terminated.  */
		  end[1] = 0;
		else
		  {
		    debug (0, "tcb_mem_read failed.");
		    buffer[0] = 0;
		  }

		if (fd < 0)
		  debug (0, "opening %s failed.", buffer);
	      }

	    if (lookup_fd (tid, fd, buffer, sizeof (buffer)))
	      {
		debug (4, "%d: %s;%s;%s: %s (%s, %c%c) -> %d",
		       (int) tid,
		       tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1,
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

		int i;
		for (i = 0; i < LIBRARY_COUNT; i ++)
		  if (g_str_has_prefix (buffer, library_patches[i].filename))
		    tcb->pcb->lib_fd[i] = fd;
	      }
	  }

	  break;

	case __NR_close:
	  {
	    if (! syscall_entry)
	      break;

	    int fd = (int) ARG1;

	    char buffer[1024];
	    if (lookup_fd (tid, fd, buffer, sizeof (buffer)))
	      /* There is little reason to believe that a close on a
		 valid file descriptor will fail.  Don't save and wait
		 for the exit.  Marshall now.  */
	      {
		debug (4, "%d: %s;%s;%s: close (%d) -> %s",
		       (int) tid,
		       tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1,
		       /* fd.  */
		       fd, buffer);

		int i;
		for (i = 0; i < LIBRARY_COUNT; i ++)
		  if (fd == tcb->pcb->lib_fd[i])
		    {
		      debug (4, "lib %s closed",
			     library_patches[i].filename);
		      tcb->pcb->lib_fd[i] = -1;
		    }

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
	      debug (0, "%d: %s;%s;%s: close (%d)",
		     (int) tid,
		     tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1,
		     fd);
	  }

	  break;

#ifdef __NR_mmap2
	case __NR_mmap2:
#else
	case __NR_mmap:
#endif
	  if (! syscall_entry)
	    {
	      uintptr_t addr = ARG1;
	      size_t length = ARG2;
	      int prot = ARG3;
	      int flags = ARG4;
	      int fd = ARG5;
	      uintptr_t file_offset = ARG6;
	      uintptr_t map_addr = RET;

	      struct library_patch *lib = NULL;
	      if (fd != -1)
		{
		  int i;
		  for (i = 0; i < LIBRARY_COUNT; i ++)
		    if (fd == tcb->pcb->lib_fd[i])
		      lib = &library_patches[i];
		}

	      debug (4, "%d: %s;%s;%s: "
		     "mmap (%"PRIxPTR", %x,%s%s%s (0x%x), 0x%x, "
		     "%d (= %s), %"PRIxPTR") -> %"PRIxPTR,
		     tid,
		     tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1,
		     addr, (int) length,
		     prot & PROT_READ ? " READ" : "",
		     prot & PROT_WRITE ? " WRITE" : "",
		     prot & PROT_EXEC ? " EXEC" : "",
		     prot, flags, fd,
		     lib ? lib->filename : "other",
		     file_offset, map_addr);

	      if (lib && (prot & PROT_EXEC))
		thread_apply_patches (tcb);
	    }
	  break;

	case __NR_munmap:
	  if (! syscall_entry)
	    /* On x86-64, the loader maps the executable section of
	       libc and then shortly later unmaps the upper half.
	       Why?  Unknown.  We (try to) patch it once this
	       occurs.  */
	    {
	      debug (4, "munmap (0x%"PRIxPTR", 0x%"PRIxPTR" "
		     "(=> 0x%"PRIxPTR") => %"PRIdPTR,
		     (uintptr_t) ARG1, (uintptr_t) ARG2,
		     (uintptr_t) (ARG1 + ARG2), (uintptr_t) RET);
	      if (RET == 0 && ! pcb_patched (tcb->pcb))
		thread_apply_patches (tcb);
	    }
	  break;

	case __NR_unlink:
	case __NR_unlinkat:
	case __NR_rmdir:
	case __NR_rename:
	case __NR_renameat:
	  if (syscall_entry)
	    {
	      uintptr_t filename;
	      if (syscall == __NR_unlinkat || syscall == __NR_renameat)
		filename = ARG2;
	      else
		filename = ARG1;

	      char buffer[1024];
	      char *end;
	      if ((end = tcb_mem_read (tcb, filename,
				       buffer, sizeof (buffer) - 1, true)))
		{
		  end[1] = 0;

		  char *p;
		  if (buffer[0] == '/')
		    /* Absolute path.  */
		    p = g_strdup_printf ("/proc/%d/root/%s", tid, buffer);
		  else if (syscall != __NR_renameat || ARG1 == AT_FDCWD)
		    /* relative path.  */
		    p = g_strdup_printf ("/proc/%d/cwd/%s", tid, buffer);
		  else
		    /* renameat, relative path.  */
		    p = g_strdup_printf ("/proc/%d/fd/%d/%s",
					 (int) tid, (int) ARG3, buffer);

		  tcb->saved_src = canonicalize_file_name (p);
		  g_free (p);

		  if (tcb->saved_src)
		    {
		      tcb->saved_stat = g_malloc (sizeof (struct stat));
		      if (stat (tcb->saved_src, tcb->saved_stat) < 0)
			debug (4, "Failed to stat %s: %m", tcb->saved_src);
		    }
		}
	      else
		debug (0, "tcb_mem_read failed.");

	      debug (4, "%d: %s;%s;%s: %s (%s)",
		     (int) tcb->tid,
		     tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1,
		     syscall_str (syscall), tcb->saved_src);
	    }
	  else if (syscall == __NR_unlink
		   || syscall == __NR_unlinkat
		   || syscall == __NR_rmdir)
	    {
	      assert (! syscall_entry);

	      debug (4, "%d: %s;%s;%s: %s (%s) -> %d",
		     (int) tcb->tid,
		     tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1,
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
	  else if (syscall == __NR_rename || syscall == __NR_renameat)
	    {
	      assert (! syscall_entry);

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
		  char *end;
		  if ((end = tcb_mem_read (tcb, filename,
					   buffer, sizeof (buffer) - 1, true)))
		    {
		      end[1] = 0;

		      char *p;
		      if (buffer[0] == '/')
			/* Absolute path.  */
			p = g_strdup_printf ("/proc/%d/root/%s", tid, buffer);
		      else if (syscall != __NR_renameat || ARG1 == AT_FDCWD)
			/* relative path.  */
			p = g_strdup_printf ("/proc/%d/cwd/%s", tid, buffer);
		      else
			/* renameat, relative path.  */
			p = g_strdup_printf ("/proc/%d/fd/%d/%s",
					     (int) tid, (int) ARG3, buffer);
		      dest = canonicalize_file_name (p);
		      g_free (p);
		    }
		  else
		    debug (0, "tcb_mem_read failed.");

		  debug (4, "%d: %s;%s;%s: %s (%s, %s) -> %d",
			 tcb->tid,
			 tcb->pcb->exe, tcb->pcb->arg0, tcb->pcb->arg1,
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

	  break;
	}

      if (pcb_patched (tcb->pcb) && ! syscall_entry)
	/* The process is patched and we've processed the system call
	   entry and exit.  We don't intercept system calls
	   anymore.  */
	ptrace_op = PTRACE_CONT;

    out:
      load_shed_maybe ();

      debug (4, "ptrace(%s, %d, sig: %d)",
	     ptrace_op == PTRACE_CONT ? "CONT"
	     : ptrace_op == PTRACE_SYSCALL ? "SYSCALL" : "UNKNOWN",
	     tid, signo);
      if (ptrace (ptrace_op, tid, 0, (void *) (uintptr_t) signo) < 0)
	{
	  debug (0, "Resuming %d: %m", tid);
	  /* The process likely disappeared, perhaps violently.  */
	  if (tcb)
	    {
	      debug (4, "Detaching %d", (int) tid);
	      thread_untrace (tcb, true);
	      tcb = NULL;
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
  pcbs = g_hash_table_new (g_direct_hash, g_direct_equal);
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
      "/home",
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
    case WC_PROCESS_TRACING_CB:
      debug (0, DEBUG_BOLD ("%d(%d): %s;%s;%s %s."),
	     cb->top_levels_pid, cb->actor_pid,
	     cb->top_levels_exe, cb->top_levels_arg0, cb->top_levels_arg1,
	     cb->cb == WC_PROCESS_TRACING_CB && cb->tracing.added
	     ? "tracing" : "exited");
      return;
    }

  debug (0, "%d(%d): %s;%s;%s: %s (%s%s%s, "BYTES_FMT")",
	 cb->top_levels_pid, cb->actor_pid,
	 cb->top_levels_exe, cb->top_levels_arg0, cb->top_levels_arg1,
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
