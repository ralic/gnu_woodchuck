#include <inttypes.h>
#include <stdint.h>

#ifndef UTIL_H
#define UTIL_H

#define MAX(x, y)				\
  ({						\
    typeof (x) max_x = (x);			\
    typeof (x) max_y = (y);			\
    if (max_x < max_y)				\
      max_x = max_y;				\
    max_x;					\
  })

#define MIN(x, y)				\
  ({						\
    typeof (x) min_x = (x);			\
    typeof (x) min_y = (y);			\
    if (min_x > min_y)				\
      min_x = min_y;				\
    min_x;					\
  })


/* Return the time since the epoch with millisecond resolution.  */
static inline uint64_t
now (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return (uint64_t) tv.tv_sec * 1000ULL
    + (uint64_t) tv.tv_usec / 1000ULL;
}

#define TIME_FMT "%"PRId64" %s"
#define TIME_PRINTF(ms)				\
  ({						\
    int64_t ms_ = (ms);				\
    int neg = ms_ < 0;				\
    if (neg)					\
      ms_ = -ms_;				\
						\
    if (ms_ > 10 * 24 * 60 * 60 * 1000)		\
      ms_ /= 24 * 60 * 60 * 1000;		\
    else if (ms_ > 10 * 60 * 60 * 1000)		\
      ms_ /= 60 * 60 * 1000;			\
    else if (ms_ > 10 * 60 * 1000)		\
      ms_ /= 60 * 1000;				\
    else if (ms_ > 10 * 1000)			\
      ms_ /= 1000;				\
						\
    if (neg)					\
      ms_ = -ms_;				\
    ms_;					\
  }),						\
  ({						\
    int64_t ms_ = (ms);				\
    if (ms_ < 0)				\
  	ms_ = -ms_;				\
    char *s_ = "ms";				\
    if (ms_ > 10 * 24 * 60 * 60 * 1000)		\
      s_ = "days";				\
    else if (ms_ > 10 * 60 * 60 * 1000)		\
      s_ = "hours";				\
    else if (ms_ > 10 * 60 * 1000)		\
      s_ = "mins";				\
    else if (ms_ > 10 * 1000)			\
      s_ = "secs";				\
						\
    s_;						\
  })

#define BYTES_FMT "%"PRId64" %s"
#define BYTES_PRINTF(bytes)			\
  ({						\
    int64_t b_ = (bytes);			\
    int neg = b_ < 0;				\
    if (neg)					\
      b_ = -b_;					\
						\
    if (b_ > 10ULL * 1024 * 1024 * 1024)	\
      b_ /= 1024 * 1024 * 1024;			\
    else if (b_ > 10 * 1024 * 1024)		\
      b_ /= 1024 * 1024;			\
    else if (b_ > 10 * 1024)			\
      b_ /= 1024;				\
						\
    if (neg)					\
      b_ = -b_;					\
    b_;						\
  }),						\
  ({						\
    int64_t b_ = (bytes);			\
    if (b_ < 0)					\
      b_ = -b_;					\
    char *s_ = "bytes";				\
    if (b_ > 10ULL * 1024 * 1024 * 1024)	\
      s_ = "gb";				\
    else if (b_ > 10 * 1024 * 1024)		\
      s_ = "mb";				\
    else if (b_ > 10 * 1024)			\
      s_ = "kb";				\
    s_;						\
  })

/* Compute the approximate value of the log_2(i).  Return that times
   2.  */
static inline int
i2log2 (uint64_t i)
{
  if (i == 0)
    return 2;

  int highest_set = (sizeof (i) * 8) - __builtin_clzll (i);

  int l = highest_set * 2;
  if (highest_set > 1 && (i & (1 << (highest_set - 2))))
    /* Second highest bit set.  */
    l ++;

  return l;
}

/* A convenience function.  */
static inline int
sqlite3_exec_printf (sqlite3 *db, const char *sql,
		     int (*callback)(void*,int,char**,char**), void *cookie,
		     char **errmsg, ...)
{
  va_list ap;
  va_start (ap, errmsg);

  char *s = sqlite3_vmprintf (sql, ap);
  debug (5, "%s", s);

  int ret = sqlite3_exec (db, s, callback, cookie, errmsg);

  sqlite3_free (s);

  va_end (ap);

  return ret;
}

#endif
