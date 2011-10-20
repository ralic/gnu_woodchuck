#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <execinfo.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>

/* The file to send output to (only used if logging to a file, see
   e.g. LOG_TO_DB).  */
#define DEBUG_OUTPUT_FILENAME "debug-output.db"

/* Convenient debugging macros.  */
#define DEBUG_BOLD_BEGIN "\033[01;31m"
#define DEBUG_BOLD_END "\033[00m"
#define DEBUG_BOLD(text) DEBUG_BOLD_BEGIN text DEBUG_BOLD_END

#if defined(DEBUG_ELIDE)
# if DEBUG_ELIDE + 0 == 0
#  define do_debug(level) if (0)
# endif
#endif

#ifndef do_debug
#ifndef MAX
# define MAX(x, y)				\
  ({						\
    typeof (x) max_x = (x);			\
    typeof (x) max_y = (y);			\
    if (max_x < max_y)				\
      max_x = max_y;				\
    max_x;					\
  })
#endif

# ifndef DEBUG_COND
extern int output_debug_global;
extern __thread int output_debug;
#  define OUTPUT_DEBUG MAX (output_debug, output_debug_global)
#  ifdef DEBUG_ELIDE
/* We elide some code at compile time.  */
#   define DEBUG_COND(level)				\
  ((level) <= OUTPUT_DEBUG && (level) < DEBUG_ELIDE)
#  else
/* The default DEBUG_COND is LEVEL <= output_debug.  */
#   define DEBUG_COND(level) ((level) <= OUTPUT_DEBUG)
#  endif /* DEBUG_ELIDE.  */
# endif /* DEBUG_COND.  */

# define do_debug(level) if (DEBUG_COND(level))

#endif /* do_debug.  */

extern void debug_(const char *file, const char *function, int line,
		   void *return_address,
		   int level, bool async,
		   const char *fmt, ...)
  __attribute__ ((format (printf, 7, 8)));

/* Print a debug message if DEBUG_COND is true.  */
#define debug_full(level, async, fmt, ...)				\
  do									\
    {									\
      do_debug (level)							\
        {								\
	  const char *__d_fn = __builtin_strrchr (__FILE__, '/');	\
	  debug_ (__d_fn ? __d_fn + 1 : __FILE__,			\
		  __func__, __LINE__,					\
		  __builtin_return_address (0),				\
		  level, async, fmt, ##__VA_ARGS__);			\
	}								\
    }									\
  while (0)

#define debug_sync(level, fmt, ...)		\
  debug_full(level, false, fmt, ##__VA_ARGS__)

#define debug_async(level, fmt, ...)		\
  debug_full(level, true, fmt, ##__VA_ARGS__)

/* When using debug, if the debug level is 0 or less than 2 below the
   maximum, we write synchronously.  */
#define DEBUG_ASYNC_THRESHOLD_DELTA 2
#define debug(level, fmt, ...)						\
  debug_full(level,							\
	     ((level) == 0						\
	      || DEBUG_COND ((level) + DEBUG_ASYNC_THRESHOLD_DELTA))	\
	     ? false : true,						\
	     fmt, ##__VA_ARGS__)

/* Returns the absolute filename of the file used for debugging
   output, or NULL if not sending output to a file.  */
extern const char *debug_init_ ();

#define debug_init()							\
  do_debug (0)								\
    debug_init_ ();							\
  else									\
    {									\
      extern int output_debug_global __attribute__ ((weak));		\
      if (&output_debug_global)						\
	fprintf (stderr, "%s debugging disabled but not everywhere.",	\
		 __FILE__);						\
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
