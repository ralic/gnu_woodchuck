#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>

#include "debug.h"

#if !defined(DEBUG_ELIDE)
int output_debug = 3;
#endif

void
blob_dump (const char *buffer, int bytes)
{
  int skip = 0;

  int chunk_size = sizeof (uint32_t) * 4;
  int chunk;
  int offset = 0;
  for (; bytes > 0; bytes -= chunk, buffer += chunk, offset += chunk)
    {
      chunk = bytes < chunk_size ? bytes : chunk_size;

      bool all_zeros = true;
      int i;
      for (i = 0; i < chunk; i ++)
	if (buffer[i])
	  {
	    all_zeros = false;
	    break;
	  }

      if (all_zeros)
	{
	  skip += chunk;
	  continue;
	}

      if (skip)
	{
	  printf ("  Skipped %d zero bytes.\n", skip);
	  skip = 0;
	}

      /* Print as hex.  */
      printf ("  %d:", offset);
      for (i = 0; i < chunk; i ++)
	{
	  if (i % sizeof (uint32_t) == 0)
	    printf (" ");
	  printf ("%02x", (unsigned char) buffer[i]);
	}

      printf ("  ");

      /* Print printable characters.  */
      for (i = 0; i < chunk; i ++)
	{
	  if (i % sizeof (uint32_t) == 0)
	    printf (" ");
	  putchar (isprint (buffer[i]) ? buffer[i] : '.');
	}
      putchar ('\n');
    }
  if (skip)
    printf ("  Skipped %d zero bytes.\n", skip);
}
