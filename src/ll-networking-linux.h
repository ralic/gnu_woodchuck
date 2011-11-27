/* linux.h - Network monitor low-level Linux support.
   Copyright 2010 Neal H. Walfield <neal@walfield.org>

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

#ifndef NETCZAR_LL_NETWORKING_LINUX_H
#define NETCZAR_LL_NETWORKING_LINUX_H

#include <stdbool.h>
#include "config.h"

/* Return the interface which has the IP address IP.  Caller must free
   the returned string.  */
#if HAVE_ICD2
# define NEED_ip_to_interface 1
# include <arpa/inet.h>
extern char *ip_to_interface (in_addr_t ip);
#endif

/* Return the IP associated with an interface.  */
#if HAVE_ICD2
# define NEED_interface_to_ip 1
# include <arpa/inet.h>
extern in_addr_t interface_to_ip (const char *interface);
#endif


/* Return whether an interface supports wireless extensions.  */
#if HAVE_ICD2
# define NEED_interface_is_wifi 1
extern bool interface_is_wifi (const char *interface);
#endif


/* Given an interface (e.g., wlan0), return the current SSID.  Caller
   must free the returned string.  */
#if HAVE_ICD2
# define NEED_interface_to_ssid 1
extern char *interface_to_ssid (const char *interface);
#endif


/* Split LINE, a line of text, into at most FIELDS_SIZE tokens.
   Tokens are separated by 1 or more while spaces (space or tab).
   Returns the number of fields.  Modifies LINE in place.
   NUL-terminates the tokens.  */
extern int split_line (char *line, int fields_size, char *fields[]);


/* Interate over each line in /proc/net/dev, which has the form:

     <Interface>: <RX bytes> <RX packets> <RX errors> <RX drop> <RX fifo>
                  <RX frame> <RX compressed> <RX multicast>
                  <TX ...>

   Stops early if CB returns false.  */
#if HAVE_ICD2 || HAVE_NETWORK_MANAGER
#  define NEED_for_each_proc_net_dev 1
extern void for_each_proc_net_dev (bool (*cb) (char *interface,
					       char *rest_of_line));
#endif

/* Iterate over each line in /proc/net/route, which has the form:

     <Interface> <Destination> <Gateway> <Flags> <RefCnt> <Use>
     <Metric> <Mask> <MTU> <Window> <IRTT>

   Stops early if CB returns false.  */
#if HAVE_ICD2
# define NEED_for_each_proc_net_route 1
extern void for_each_proc_net_route (bool (*cb) (char *interface,
						 char *rest_of_line));
#endif


/* Call cb for each line in /proc/net/arp, which has the form:

     <IP> <HW type> <Flags> <HW Address> <Mask> <Device>

   Stops early if CB returns false.  */
#if HAVE_ICD2 || HAVE_NETWORK_MANAGER
# define NEED_for_each_proc_net_arp 1
extern void for_each_proc_net_arp (bool (*cb) (char *ip, char *rest_of_line));
#endif

#endif
