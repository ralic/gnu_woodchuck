#include <inttypes.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#ifndef UTIL_H
#define UTIL_H

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
#ifndef MAX3
# define MAX3(x, y, z) MAX (x, MAX (y, z))
#endif
#ifndef MAX4
# define MAX4(x, y, z, a) MAX (MAX (x, a), MAX (y, z))
#endif

#ifndef MIN
# define MIN(x, y)				\
  ({						\
    typeof (x) min_x = (x);			\
    typeof (x) min_y = (y);			\
    if (min_x > min_y)				\
      min_x = min_y;				\
    min_x;					\
  })
#endif

/* Return the time since the epoch with millisecond resolution.  */
static inline uint64_t
now (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return (uint64_t) tv.tv_sec * 1000ULL
    + (uint64_t) tv.tv_usec / 1000ULL;
}

static inline struct tm
now_tm (void)
{
  time_t t = time (NULL);
  struct tm tm;
  localtime_r (&t, &tm);
  return tm;
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

#endif
