#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <execinfo.h>
#include <stdlib.h>
#include <time.h>

/* Convenient debugging macros.  */
#define DEBUG_BOLD(text) "\033[01;31m" text "\033[00m"

#if defined(DEBUG_ELIDE)
# if DEBUG_ELIDE + 0 == 0
#  define do_debug(level) if (0)
# endif
#endif

#ifndef do_debug
# ifndef DEBUG_COND
extern int output_debug;
#  ifdef DEBUG_ELIDE
/* We elide some code at compile time.  */
#   define DEBUG_COND(level)				\
  ((level) <= output_debug && (level) < DEBUG_ELIDE)
#  else
/* The default DEBUG_COND is LEVEL <= output_debug.  */
#   define DEBUG_COND(level) ((level) <= output_debug)
#  endif /* DEBUG_ELIDE.  */
# endif /* DEBUG_COND.  */

# define do_debug(level) if (DEBUG_COND(level))

#endif /* do_debug.  */

/* Print a debug message if DEBUG_COND is true.  */
#define debug(level, fmt, ...)						\
  do									\
    {									\
      do_debug (level)							\
        {								\
	  time_t __t = time (NULL);					\
	  struct tm __tm;						\
	  localtime_r (&__t, &__tm);					\
									\
	  fprintf (stderr, "%d.%d.%d %d:%02d.%02d:%s:%d:(%p): " fmt "\n", \
		   1900 + __tm.tm_year, __tm.tm_mday, __tm.tm_mon + 1,	\
		   __tm.tm_hour, __tm.tm_min, __tm.tm_sec,		\
		   __func__, __LINE__,					\
		   __builtin_return_address (0),			\
		   ##__VA_ARGS__);					\
	  fflush (stderr);						\
	}								\
    }									\
  while (0)

#define debug_init()							\
  do_debug (0)								\
    ;									\
  else									\
    {									\
      extern int output_debug __attribute__ ((weak));			\
      if (&output_debug)						\
	fprintf (stderr, "%s debugging disabled but not everywhere.", __FILE__);	\
    }



#define assertx(__ax_expr, __ax_fmt, ...)				\
  do {									\
    if (! (__ax_expr))							\
      {									\
	printf ("%s:%s:%d: %s failed",					\
		__FILE__, __func__, __LINE__, #__ax_expr);		\
	if ((__ax_fmt) && *(__ax_fmt))					\
	  printf (": " __ax_fmt, ##__VA_ARGS__);			\
	printf ("\n");							\
	fflush (stdout);						\
									\
	void *array[10];						\
	size_t size;							\
	char **strings;							\
	size_t i;							\
									\
	size = backtrace (array, 10);					\
	strings = backtrace_symbols (array, size);			\
									\
	for (i = 0; i < size; i++)					\
          printf ("%s\n", strings[i]);					\
									\
	free (strings);							\
									\
	abort ();							\
      }									\
  } while (0)

#ifndef build_assert
/* The build assert functions are taken from gnulib's
   http://git.sv.gnu.org/gitweb/?p=gnulib.git;a=blob_plain;f=lib/verify.h
   .  */
# ifdef __cplusplus
template <int w>
struct build_assert_type__
{
  unsigned int build_assert_error_if_negative_size__: w;
};
#  define build_assert_true(R)                          \
  (!!sizeof (build_assert_type__<(R) ? 1 : -1>))
# else
#  define build_assert_true(R)						\
  (!!sizeof								\
   (struct								\
   {									\
     unsigned int build_assert_error_if_negative_size__ : (R) ? 1 : -1;	\
   }))
# endif

# define build_assert(R)                                                \
  extern int (* build_assert_function__ (void)) [build_assert_true (R)]

#endif  /* ! built_assert  */

/* Dump (potentially) binary data to stdout.  */
extern void blob_dump (const char *buffer, int bytes);

#endif
