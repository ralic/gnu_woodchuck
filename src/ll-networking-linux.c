/* ll-networking-linux.h - Network monitor low-level Linux support.
   Copyright 2010 Neal H. Walfield <neal@walfield.org>

   This file is part of Netczar.

   Netczar is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   Netczar is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "ll-networking-linux.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if.h>

#include "debug.h"

#if NEED_ip_to_interface || NEED_interface_to_ip || NEED_interface_is_wifi \
    || NEED_interface_to_ssid
static int
ioctl_fd (void)
{
  static int fd;
  if (fd == 0 || fd < 0)
    {
      fd = socket (AF_INET, SOCK_DGRAM, 0);
      if (fd < 0)
	debug (0, "Failed to create socket: %m");
    }

  return fd;
}
#endif

#if NEED_ip_to_interface
char *
ip_to_interface (in_addr_t ip)
{
  struct ifconf ifconf;
  memset (&ifconf, 0, sizeof(ifconf));
  struct ifreq ifreqs[100];
  ifconf.ifc_buf = (char *) ifreqs;
  ifconf.ifc_len = sizeof (ifreqs);

  int ret = ioctl (ioctl_fd (), SIOCGIFCONF, (char *) &ifconf);
  if (ret < 0)
    {
      debug (0, "SIOCGIFCONF: %m");
      return NULL;
    }

  debug (5, "Looking up %s (%u)", inet_ntoa ((struct in_addr) { ip }), ip);

  int i;
  for (i = 0; i < ifconf.ifc_len / sizeof (struct ifreq); i ++)
    {
      struct ifreq *ifreq = &ifreqs[i];
      if (ifreq->ifr_addr.sa_family == AF_INET)
	{
	  struct sockaddr_in *if_addr = (void *) &ifreq->ifr_addr;
	  debug (5, "  %s: %s (%u)",
		 ifreq->ifr_name, inet_ntoa (if_addr->sin_addr),
		 if_addr->sin_addr.s_addr);

	  if (ip == if_addr->sin_addr.s_addr)
	    return strdup (ifreq->ifr_name);
	}
    }

  return NULL;
}
#endif

#if NEED_interface_to_ip
in_addr_t
interface_to_ip (const char *interface)
{
  struct ifreq ifreq;
  strncpy (ifreq.ifr_name, interface, sizeof (ifreq.ifr_name));

  int ret = ioctl (ioctl_fd (), SIOCGIFADDR, (char *) &ifreq);
  if (ret < 0)
    {
      debug (0, "SIOCGIFCONF: %m");
      return INADDR_NONE;
    }

  if (ifreq.ifr_addr.sa_family == AF_INET)
    {
      struct sockaddr_in *if_addr = (void *) &ifreq.ifr_addr;
      return if_addr->sin_addr.s_addr;
    }
  return INADDR_NONE;
}
#endif

#if NEED_interface_is_wifi
#include <linux/wireless.h>

/* Return whether an interface support wireless extensions.  */
bool
interface_is_wifi (const char *interface)
{
  struct iwreq iwreq;
  memset (&iwreq, 0, sizeof (iwreq));
  strncpy (iwreq.ifr_name, interface, IFNAMSIZ);

  int fd = ioctl_fd ();
  if (fd >= 0 && ioctl (fd, SIOCGIWNAME, &iwreq) >= 0)
    return true;

  debug (0, "ioctl (SIOCGIWNAME): %m");
  return false;
}
#endif

#if NEED_interface_to_ssid
#include <linux/wireless.h>

char *
interface_to_ssid (const char *interface)
{
  char *ssid = NULL;

  struct iwreq iwreq;
  memset (&iwreq, 0, sizeof (iwreq));
  char buffer[IW_ESSID_MAX_SIZE + 1];
  strncpy (iwreq.ifr_name, interface, IFNAMSIZ);
  iwreq.u.essid.pointer = buffer;
  iwreq.u.essid.length = sizeof (buffer);
  iwreq.u.essid.flags = 0;

  int fd = ioctl_fd ();
  if (fd >= 0 && ioctl (fd, SIOCGIWESSID, &iwreq) >= 0)
    ssid = strdup (buffer);
  else
    debug (0, "ioctl (SIOCGIWESSID): %m");

  return ssid;
}
#endif

int
split_line (char *line, int fields_size, char *fields[])
{
  int d = 5;
  debug (d, "Splitting '%s' into at most %d fields",
	 line, fields_size);

  int i;
  char *p;
  for (i = 0, p = line;
       p && *p && i < fields_size;
       i ++)
    {
      debug (d, "Remaining: %s", p);

      /* Skip spaces.  Replace them with NULs to terminate the
	 line.  */
      if (*p == ' ' || *p == '\t')
	{
	  *p = 0;
	  p ++;
	  debug (d, "NUL-terminating previous token.");
	}

      /* Skip spaces.  */
      int l = strspn (p, " \t");
      debug (d, "Skipping %d space characters ('%.*s')", l, l, p);
      p += l;

      fields[i] = p;

      l = strcspn (p, " \t");
      debug (d, "Token '%.*s' has %d characters",
	     l, p, l);
      p += l;
    }
  if (p && (*p == ' ' || *p == '\t' || *p == '\n'))
    /* NUL-terminate the last token.  */
    *p = 0;

  debug (d, "Got %d tokens", i);

  return i;
}

#if NEED_for_each_proc_net_dev
void
for_each_proc_net_dev (bool (*cb) (char *interface, char *rest_of_line))
{
  FILE *f = fopen ("/proc/net/dev", "r");
  if (! f)
    {
      debug (0, "Failed to open /proc/net/dev: %m");
      return;
    }

  char *line = NULL;
  size_t line_bytes = 0;

  int lines = 0;
  int l;
  while ((l = getline (&line, &line_bytes, f)) != -1)
    {
      if (++ lines <= 2)
	/* First 2 lines are header information.  */
	continue;

      char *stats = strchr (line, ':');
      if (! stats)
	/* Hmm... bad line.  */
	continue;

      char *interface = line;
      while (*interface == ' ')
	interface ++;

      /* Null terminate the interface.  */
      *stats = 0;
      stats ++;

      if (! cb (interface, stats))
	break;
    }

  free (line);
  fclose (f);
}
#endif

#if NEED_for_each_proc_net_route
void
for_each_proc_net_route (bool (*cb) (char *interface, char *rest_of_line))
{
  FILE *f = fopen ("/proc/net/route", "r");
  if (! f)
    {
      debug (0, "Failed to open /proc/net/route: %m");
      return;
    }

  char *line = NULL;
  size_t line_bytes = 0;

  int lines = 0;
  int l;
  while ((l = getline (&line, &line_bytes, f)) != -1)
    {
      if (++ lines <= 1)
	/* First line is header information.  */
	continue;

      char *interface = line;

      int l = strcspn (line, " \t");
      if (! l)
	/* Hmm... bad line.  */
	continue;

      char *rest = line + l;

      /* Null terminate the interface.  */
      *rest = 0;
      rest ++;

      if (! cb (interface, rest))
	break;
    }

  free (line);
  fclose (f);
}
#endif

#if NEED_for_each_proc_net_arp
void
for_each_proc_net_arp (bool (*cb) (char *ip, char *rest_of_line))
{
  FILE *f = fopen ("/proc/net/arp", "r");
  if (! f)
    {
      debug (0, "Failed to open /proc/net/arp: %m");
      return;
    }

  /* A line has the format:

     IP Address, HW type, Flags, HW address, Mask, Device
  */

  char *line = NULL;
  size_t line_bytes = 0;

  int lines = 0;
  int l;
  while ((l = getline (&line, &line_bytes, f)) != -1)
    {
      if (++ lines <= 1)
	/* First line is header information.  */
	continue;

      char *ip = line;

      char *rest = strchr (line, ' ');
      if (! rest)
	/* Hmm... bad line.  */
	continue;

      /* Null terminate the interface.  */
      *rest = 0;
      rest ++;

      if (! cb (ip, rest))
	break;
    }

  free (line);
  fclose (f);
}
#endif
