#ifndef NETCZAR_SCHEDULE_H
#define NETCZAR_SCHEDULE_H

/* Applications associate schedules with actions.  In its simpliest
   form, a schedule indiactes when an action should be performed,
   e.g., download the feed every 6 hours.  Generally, there is a fair
   amount of flexibility: actions have different utilities, i.e.,
   value to the user, and different constraints.  For instance, a
   PodCast should be updated four times per day.  This does not mean
   that the PodCast should be updated exactly every 6 hours.  Instead,
   the download can be delayed several hours until inexpensive
   connectivity is available, e.g., WLAN vs. GSM.  Further, if Netczar
   learns that WLAN is likely available and not available at certain
   times, it can schedule a pending update a bit early.  To this end,
   we provide a number of ways to describe when an action should be
   scheduled.

   The user can influence when schedules are executed.  For instace,
   if GSM connectivity is very expensive, then no non-manual downloads
   may be performed.  This can be configured on a system-wide basis,
   application basis and download-type basis.*/

#include <stdint.h>

struct netczar_schedule;
typedef struct netczar_schedule *netczar_schedule_t;

/* Allocate a new, default schedule.  */
netczar_schedule_t netczar_schedule_new (void);

/* Free a schedule.  */
void netczar_schedule_free (netczar_schedule_t schedule);

/* Copy a schedule.  Both the original and the new must be freed using
   netczar_schedule_free.  */
netczar_schedule_t netczar_schedule_copy (const netczar_schedule_t schedule);


/* Some default schedules.  The trigger is unset and the estimated
   size may be inappripriate.  To use, first copy the schedule
   (netczar_schedule_copy), then modify.  */
extern const netczar_schedule_t netczar_schedule_podcast_headers;
extern const netczar_schedule_t netczar_schedule_podcast_content;
extern const netczar_schedule_t netczar_schedule_email_headers;
extern const netczar_schedule_t netczar_schedule_email_body;
extern const netczar_schedule_t netczar_schedule_email_attachments;


/* When to trigger the action.  All times are in seconds.  DELTA is
   relative to now.  EARLIEST and LATEST are relative to DELTA.  DELTA
   is the desired time at which action should be performed.  EARLIEST
   is the number of seconds prior to DELTA that it is permissable to
   perform the action.  LATEST is the number of seconds after DELTA
   after which it makes no sense to perform the action any more.
   Setting EARLIEST or LATEST to -1 disables the constraints.  */
void netczar_schedule_trigger_set (netczar_schedule_t schedule,
				   uint64_t delta,
				   uint64_t earliest,
				   uint64_t latest);

void netczar_schedule_trigger_get (netczar_schedule_t schedule,
				   uint64_t *delta,
				   uint64_t *earliest,
				   uint64_t *latest);

/* Whether the action should be executed once or repeated.  */
enum netczar_schedule_frequency
  {
    NETCZAR_SCHEDULE_FREQUENCY_ONE_SHOT,
    NETCZAR_SCHEDULE_FREQUENCY_REPEAT,
    NETCZAR_SCHEDULE_FREQUENCY_DEFAULT = NETCZAR_SCHEDULE_FREQUENCY_ONE_SHOT,
  };

void netczar_schedule_frequency_set (netczar_schedule_t schedule,
				     enum netczar_schedule_frequency freq);

enum netczar_schedule_frequency
  netczar_schedule_frequency_get (netczar_schedule_t schedule);


enum netczar_schedule_precision
  {
    NETCZAR_SCHEDULE_PRECISION_SECOND,
    NETCZAR_SCHEDULE_PRECISION_MINUTE,
    NETCZAR_SCHEDULE_PRECISION_15_MINUTES,
    NETCZAR_SCHEDULE_PRECISION_HOUR,
    NETCZAR_SCHEDULE_PRECISION_6_HOURS,
    NETCZAR_SCHEDULE_PRECISION_DAY,
    NETCZAR_SCHEDULE_PRECISION_WEEK,
    NETCZAR_SCHEDULE_PRECISION_DEFAULT = NETCZAR_SCHEDULE_PRECISION_HOUR,
  };

void netczar_schedule_precision_set (netczar_schedule_t schedule,
				     enum netczar_schedule_precision prec);

enum netczar_schedule_precision
  netczar_schedule_precision_get (netczar_schedule_t precision);


enum netczar_schedule_type
  {
    NETCZAR_SCHEDULE_TYPE_USER_REQUEST,
    NETCZAR_SCHEDULE_TYPE_AUTOMATIC,
  };

void netczar_schedule_type_set (netczar_schedule_t schedule,
				enum netczar_schedule_type type);

enum netczar_schedule_type
  netczar_schedule_type_get (netczar_schedule_t schedule);


enum netczar_schedule_priority
  {
    /* For streaming content.  */
    NETCZAR_SCHEDULE_PRIORITY_HIGH = 0,
    /* For normal downloads, e.g., when the user clicks on a link in a
       web browser.  */
    NETCZAR_SCHEDULE_PRIORITY_NORMAL = 7,
    /* For bulk downloading of metadata, e.g., Podcast headers.  */
    NETCZAR_SCHEDULE_PRIORITY_BULK_META = 11,
    /* For bulk downloads, e.g., Podcast episodes.  */
    NETCZAR_SCHEDULE_PRIORITY_BULK = 13,
    NETCZAR_SCHEDULE_PRIORITY_LOW = 15,
    NETCZAR_SCHEDULE_PRIORITY_DEFAULT = NETCZAR_SCHEDULE_PRIORITY_NORMAL,
  };

void netczar_schedule_priority_set (netczar_schedule_t schedule,
				enum netczar_schedule_priority prio);

enum netczar_schedule_priority
  netczar_schedule_priority_get (netczar_schedule_t schedule);


/* The order of the expected size of the download.  (Note: the expect
   size is 1024 << EXPECTED_SIZE).  */
enum netczar_schedule_expected_size
  {
    NETCZAR_SCHEDULE_EXPECTED_SIZE_UNKNOWN = -1,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_KB = 0,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_10_KB = 4,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_100_KB = 7,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_MB = 10,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_10_MB = 13,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_100_MB = 17,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_GB = 20,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_10_GB = 23,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_100_GB = 27,

    NETCZAR_SCHEDULE_EXPECTED_SIZE_DEFAULT
      = NETCZAR_SCHEDULE_EXPECTED_SIZE_UNKNOWN,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_SMALL
      = NETCZAR_SCHEDULE_EXPECTED_SIZE_10_KB,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_MEDIUM
      = NETCZAR_SCHEDULE_EXPECTED_SIZE_MB,
    NETCZAR_SCHEDULE_EXPECTED_SIZE_LARGE
      = NETCZAR_SCHEDULE_EXPECTED_SIZE_100_MB,
  };

/* Convert an expected size to bytes.  */
static inline uint64_t
netczar_schedule_expected_size_to_bytes (enum netczar_schedule_priority esize)
{
  if (esize == -1)
    return -1;
  return 1024ULL << esize;
}

/* Convert a number of byte to an expected size.  */
static inline uint64_t
netczar_schedule_bytes_to_expected_size (uint64_t size)
{
  if (size == -1)
    return -1;

  /* We are looking for the order.  If we have 10k - 1, we don't want
     SIZE_KB but SIZE_10_KB.  */
  size = size + size / 2;

  int i = 0;
  while (size > 0)
    {
      i ++;
      size >>= 2;
    }

  return i;
}

/* Set SCHEDULE's expected size to EXPECTED_SIZE.  */
void netczar_schedule_expected_size_set
  (netczar_schedule_t schedule,
   enum netczar_schedule_priority expected_size);

/* Return SCHEDULE's expected size.  */
enum netczar_schedule_expected_size
  netczar_schedule_expected_size_get (netczar_schedule_t schedule);

#endif
