/* smart-storage-logger-recover.c - Recover the system after a crash.
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

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/ptrace.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
#include <inttypes.h>
#include <error.h>
#include <errno.h>
#include <pthread.h>

#include <sqlite3.h>
#include <glib.h>

#include "files.h"
#include "debug.h"
#include "util.h"

static int
tkill (int tid, int sig)
{
  return syscall (__NR_tkill, tid, sig);
}

#ifdef __arm__
# define INSTRUCTION_LEN_MAX 4
#else
# define INSTRUCTION_LEN_MAX 8
#endif

/* An instruction is a sequence of bytes.  */
struct instruction
{
  int len;
  union
  {
    uint64_t word64[INSTRUCTION_LEN_MAX / sizeof (uint64_t)];
    uint32_t word32[INSTRUCTION_LEN_MAX / sizeof (uint32_t)];
    char byte[INSTRUCTION_LEN_MAX];
  };
};

#ifdef __x86_64__
static struct instruction breakpoint_ins = { 1, { .byte = { 0xCC } } };
#elif defined(__arm__)
static struct instruction breakpoint_ins = { 4, { .word32 = { 0xe7f001f0 } } };
#else
#warning Not ported to your architecture.
#endif

/* A patch describes a change.  */
struct patch
{
  /* The instruction to write.  */
  struct instruction ins;
  /* The offset.  */
  uintptr_t offset;
  /* The next patch in the change set.  */
  struct patch *next;
};

/* A hash from library names to GSList *s of <char *, struct patch *,
   struct patch *, ...>.  The first char * is the library name.  */
static GHashTable *patch_sets;

int
main (int argc, char *argv[])
{
  {
    char *log = files_logfile ("output");

    gchar *contents = NULL;
    gsize length = 0;
    if (g_file_get_contents (log, &contents, &length, NULL))
      {
	debug (0, "Last instance's output: %s (%d bytes)",
	       contents, (int) length);
	g_free (contents);
      }
    unlink (log);

    /* Redirect our stdout and stderr to the log file.  */
    int log_fd = open (log, O_WRONLY | O_CREAT, 0660);
    dup2 (log_fd, STDOUT_FILENO);
    dup2 (log_fd, STDERR_FILENO);
    if (! (log_fd == STDOUT_FILENO || log_fd == STDERR_FILENO))
      close (log_fd);

    free (log);
  }


  sqlite3 *db;
  char *db_filename = files_logfile ("process-patches");
  int err = sqlite3_open (db_filename, &db);
  if (err)
    error (1, 0, "sqlite3_open (%s): %s",
	   db_filename, sqlite3_errmsg (db));

  /* Sleep up to an hour if the database is busy...  */
  sqlite3_busy_timeout (db, 60 * 60 * 1000);

  patch_sets = g_hash_table_new (g_str_hash, g_str_equal);


  /* Read the list of patches.  */
  int read_patches_callback (void *cookie, int argc, char **argv, char **names)
  {
    /* New patch chunk.  */
    struct patch p;

    char *lib = argv[0];
    char *offset = argv[1];
    char *len = argv[2];
    char **ins = &argv[3];

    debug (4, "Patch for %s at %s, %s bytes", lib, offset, len);

    GSList *set = g_hash_table_lookup (patch_sets, lib);
    if (! set)
      /* New library.  */
      {
	char *l = g_strdup (lib);
	set = g_slist_append (NULL, l);
	g_hash_table_insert (patch_sets, l, set);
      }

    /* Get the instruction offset.  */
    char *end = NULL;
    errno = 0;
    p.offset = strtoumax (offset, &end, 16);
    if (errno || ! end || *end != '\0')
      {
	debug (0, "Error parsing '%s', not a hexadecimal number%s%s",
	       offset, errno ? ": " : "", errno ? strerror (errno) : "");
	return 0;
      }

    /* Get the instruction length.  */
    end = NULL;
    errno = 0;
    p.ins.len = strtoumax (len, &end, 16);
    if (errno || ! end || *end != '\0')
      {
	debug (0, "Error parsing '%s', not a hexadecimal number%s%s",
	       len, errno ? ": " : "", errno ? strerror (errno) : "");
	return 0;
      }

    if (! p.ins.len || p.ins.len > sizeof (p.ins.byte))
      {
	debug (0, "Bad instruction size (%d, but should be 0 < size <= %d)",
	       p.ins.len, (int) sizeof (p.ins.byte));
	return 0;
      }

    /* Get the original instruction, a series of hexadecimal-as-string
       encoded byte values.  */
    int i;
    for (i = 0; i < p.ins.len; i ++)
      {
	char *end = NULL;
	errno = 0;
	uintmax_t v = strtoumax (ins[i], &end, 16);
	if (errno || ! end || *end != '\0' || v > 256)
	  {
	    debug (0, "Error parsing '%s', not a hexadecimal number%s%s",
		   ins[i], errno ? ": " : "", errno ? strerror (errno) : "");
	    return 0;
	  }
	p.ins.byte[i] = v;
      }

    /* Add the patch chunk to the patch.  */
    struct patch *pp = malloc (sizeof (p));
    *pp = p;

    int c = g_slist_length (set);
    GSList *new_head = g_slist_insert_before (set, set->next, pp);
    assert (new_head == set);
    assert (g_slist_length (set) == c + 1);

    return 0;
  }

  char *errmsg = NULL;
  sqlite3_exec (db, "select lib, offset, len, o1, o2, o3, o4, o5, o6, o7, o8"
		" from patches;",
		read_patches_callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
      return 1;
    }

  void iter (gpointer key, gpointer value, gpointer user_data)
  {
    char *lib = key;
    GSList *patches = value;
    debug (0, "%d patches for %s", g_slist_length (patches) - 1, lib);
  }
  g_hash_table_foreach (patch_sets, iter, NULL);
  

  int unpatch_callback (void *cookie, int argc, char **argv, char **names)
  {
    int fd = * (int *) cookie;
    int pid = atoi (argv[0]);
    char *lib = argv[1];
    char *base_str = argv[2];

    debug (4, "Process %d: reverting process patches to %s (mapped at %s)",
	   (int) pid, lib, base_str);

    GSList *set = g_hash_table_lookup (patch_sets, lib);
    if (! set)
      {
	debug (0, "No patch set for library %s.", lib);
	return 0;
      }
    assert (strcmp (lib, set->data) == 0);
    int patch_set_count = g_slist_length (set) - 1;

    /* Skip the library's name.  */
    set = set->next;

    char *end = NULL;
    uintptr_t base = strtoumax (base_str, &end, 16);
    if (! base || ! end || *end != '\0')
      {
	debug (0, "Unable to extract address from '%s'", base_str);
	return 0;
      }

    int already_reverted = 0;
    for (; set; set = set->next)
      {
	struct patch *p = set->data;

	uintptr_t addr = base + p->offset;
	int len = p->ins.len;

	/* Get the instruction at the patch location.  */
	char buffer[len];
	int r = pread (fd, buffer, len, addr);
	if (r < 0)
	  {
	    debug (0, "Failed to read process %d's memory "
		   "%"PRIxPTR" + %"PRIxPTR" = %"PRIxPTR": %m",
		   pid, base, p->offset, base + p->offset);
	    return 0;
	  }
	if (r != len)
	  {
	    debug (0, "Reading process %d's memory: short read (%d of %d)",
		   pid, r, len);
	    return 0;
	  }

	if (memcmp (buffer, p->ins.byte, p->ins.len) == 0)
	  {
	    already_reverted ++;
	    debug (5, "Patch to %d at %s+%"PRIxPTR" already reverted",
		   pid, lib, p->offset);
	    continue;
	  }

	/* Patch the original instruction.  */
	struct instruction n;
	memcpy (&n, p->ins.byte, sizeof (n));
	memcpy (n.byte, breakpoint_ins.byte, breakpoint_ins.len);

	/* Check that the current value matches the patched value.  */
	if (memcmp (buffer, n.byte, p->ins.len) != 0)
	  {
	    GString *s = g_string_new ("");
	    g_string_append_printf
	      (s, "%d's memory does not contain expected value: expected",
	       pid);
	    int i;
	    for (i = 0; i < p->ins.len; i ++)
	      g_string_append_printf (s, " %02x", n.byte[i]);
	    g_string_append_printf (s, ", but got:");
	    for (i = 0; i < p->ins.len; i ++)
	      g_string_append_printf (s, " %02x", buffer[i]);

	    debug (0, "%s", s->str);
	    g_string_free (s, true);

	    return 0;
	  }

	GString *s = g_string_new ("");
	g_string_append_printf
	  (s, "%d%s@%"PRIxPTR" + %"PRIxPTR" = %"PRIxPTR" before:",
	   pid, lib, base, p->offset, base + p->offset);
	r = pread (fd, buffer, len, addr);
	int i;
	for (i = 0; i < r; i ++)
	  g_string_append_printf (s, " %02x", buffer[i]);
	g_string_append_printf (s, "(%d)", r);


	/* Do the update.  */
	int offset = addr & (sizeof (uintptr_t) - 1);
	uintptr_t a = addr - offset;
	int b = p->ins.len;
	const char *nv = p->ins.byte;
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
		value.word = ptrace (PTRACE_PEEKDATA, pid, a);
		if (errno)
		  {
		    debug (0, "Failed to read process %d's memory, "
			   "location %"PRIxPTR": %m",
			   pid, a);
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

	    if (ptrace (PTRACE_POKEDATA, pid, a, value.word) < 0)
	      {
		debug (0, "Failed to write to process %d's memory, "
		       "location %"PRIxPTR": %m",
		       pid, a);
		return false;
	      }

	    a += sizeof (uintptr_t);
	  }

	r = pread (fd, buffer, len, addr);
	g_string_append_printf (s, "; after:");
	for (i = 0; i < r; i ++)
	  g_string_append_printf (s, " %02x", buffer[i]);
	g_string_append_printf (s, "(%d)", r);

	g_string_append_printf (s, "; wanted:");
	for (i = 0; i < r; i ++)
	  g_string_append_printf (s, " %02x", p->ins.byte[i]);
	g_string_append_printf (s, "(%d)", p->ins.len);

	if (r != p->ins.len || memcmp (p->ins.byte, buffer, p->ins.len) != 0)
	  {
	    g_string_append_printf (s, DEBUG_BOLD (" MISMATCH!"));
	    debug (0, "%s", s->str);
	  }
	else
	  debug (4, "%s", s->str);

	g_string_free (s, true);
      }


    if (already_reverted)
      debug (0, "%d of %d patches to %d %s were already reverted",
	     already_reverted, patch_set_count, pid, lib);

    return 0;
  }

  void *unpatch_callback_thread (void *arg)
  {
    int pid = (int) (uintptr_t) arg;

    bool attached = false;

    int i;
    for (i = 0; i < 3; i ++)
      {
	if (! attached && ptrace (PTRACE_ATTACH, pid) == -1)
	  debug (0, "Error attaching to %d: %m", pid);
	else
	  attached = true;

	int j;
	for (j = 0; j < 3; j ++)
	  if (tkill (pid, SIGCONT) < 0)
	    debug (0, "tkill (%d, SIGCONT): %m", pid);
      }

    if (! attached)
      return (void *) -1;

    debug (0, "Waiting for %d", pid);
    int status = 0;
    pid_t result = waitpid (pid, &status, 0);
    if (result != pid)
      {
	debug (0, "Error waiting for pid %d: %m", pid);
	return (void *) -1;
      }

    char filename[32];
    snprintf (filename, sizeof (filename), "/proc/%d/mem", pid);
    filename[sizeof (filename) - 1] = 0;
    int fd = open (filename, O_RDONLY);
    if (fd < 0)
      {
	debug (0, "Error opening %s: %m", filename);
	if (errno == ENOENT)
	  return 0;
	else
	  return (void *) -1;
      }


    sqlite3 *db = NULL;
    int err = sqlite3_open (db_filename, &db);
    if (err)
      {
	debug (0, "sqlite3_open (%s): %s",
	       db_filename, sqlite3_errmsg (db));
	return (void *) -1;
      }

    /* Sleep up to an hour if the database is busy...  */
    sqlite3_busy_timeout (db, 60 * 60 * 1000);

    char *errmsg = NULL;
    err = sqlite3_exec_printf
      (db, "select pid, lib, base from processes where pid = %d;",
       unpatch_callback, (void *) &fd, &errmsg, pid);
    if (errmsg)
      {
	debug (0, "%d: %s", err, errmsg);
	sqlite3_free (errmsg);
	errmsg = NULL;
      }

    close (fd);
    if (ptrace (PTRACE_DETACH, pid, 0, SIGCONT) < 0)
      debug (0, "Detaching from %d failed: %m", pid);

    return NULL;
  }


  /* Start one thread for each process.  */
  struct thread_data
  {
    pthread_t tid;
    int pid;
  };

  GSList *threads = NULL;
  int unpatch_thread_spawn_callback (void *cookie, int argc, char **argv,
				     char **names)
  {
    int pid = atoi (argv[0]);

    struct thread_data *d = malloc (sizeof (*d));
    d->pid = pid;

    int err;
    if ((err = pthread_create (&d->tid, NULL, unpatch_callback_thread,
			       (void *) (uintptr_t) pid)))
      debug (0, "pthread_create for %s: %s", argv[0], strerror (err));
    else
      threads = g_slist_prepend (threads, d);
    return 0;
  }

  sqlite3_exec (db, "select distinct pid from processes;",
		unpatch_thread_spawn_callback, NULL, &errmsg);
  if (errmsg)
    {
      debug (0, "%d: %s", err, errmsg);
      sqlite3_free (errmsg);
      errmsg = NULL;
      return 1;
    }


  int success = 0;
  int total = g_slist_length (threads);

  time_t start = time (NULL);
  while (threads)
    {
      struct thread_data *d = threads->data;

      void *ret = NULL;

      /* Wait up to 7.5 seconds.  */
      struct timespec abstime;
      memset (&abstime, 0, sizeof (abstime));
      time_t now = time (NULL);
      abstime.tv_sec = MAX (start + 7, now + 1);
      abstime.tv_nsec = 999999999L;


      char *cmdline (int pid)
      {
	char filename[32];
	snprintf (filename, sizeof (filename), "/proc/%d/cmdline", pid);
	filename[sizeof (filename) - 1] = 0;
	gchar *cmdline = NULL;
	gsize length = 0;
	if (g_file_get_contents (filename, &cmdline, &length, NULL))
	  {
	    int i = 0;
	    for (i = 0; i < length - 1; i ++)
	      if (cmdline[i] == '\0')
		cmdline[i] = ' ';
	    cmdline[length - 1] = 0;
	  }
	return cmdline;
      }


      int err;
      if ((err = pthread_timedjoin_np (d->tid, &ret, &abstime)))
	{
	  char *c = cmdline (d->pid);
	  if (err == ETIMEDOUT)
	    {
	      debug (0, "Process %d (%s) unresponsive, killing.", d->pid, c);
	      kill (d->tid, SIGINT);
	      sleep (2);
	      kill (d->tid, SIGKILL);
	    }
	  else
	    debug (0, "Failed to join thread (process %d, %s): %s.",
		   d->pid, c, strerror (err));

	  g_free (c);
	}
      else if (ret == NULL)
	success ++;
      else
	{
	  char *c = cmdline (d->pid);
	  debug (0, "Failed to fix up process %d, %s.", d->pid, c);
	  g_free (c);
	}

      g_free (d);
      threads = g_slist_delete_link (threads, threads);
    }

  debug (success != total ? 0 : 3,
	 "Successfully recovered %d of %d processes.",
	 success, total);

  return 0;
}
