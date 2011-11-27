# Copyright (C) 2011 Neal H. Walfield <neal@walfield.org>
#
# Woodchuck is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 3, or (at
# your option) any later version.
#
# Woodchuck is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see
# <http://www.gnu.org/licenses/>.

EXTRA_DIST =
CLEANFILES =

bin_SCRIPTS = vcssync woodchuck-cli apt-woodchuck
CLEANFILES += $(bin_SCRIPTS)

do_subst = sed -e 's,[@]bindir[@],$(bindir),g' \
	-e 's,[@]PYTHON[@],$(PYTHON),g' \
	-e 's,[@]pythondir[@],$(pythondir),g'

%: %.sh Makefile
	$(do_subst) < "$<" > "$@"
	chmod +x "$@"

EXTRA_DIST += vcssync.sh woodchuck-cli.sh apt-woodchuck.sh

python_PYTHON = vcssync.py woodchuck-cli.py apt-woodchuck.py

# DBus service files
servicedir = $(DBUS_SERVICES_DIR)
service_in_files = org.woodchuck.vcssync.service.in \
	org.woodchuck.apt-woodchuck.service.in
service_DATA = $(service_in_files:.service.in=.service)

EXTRA_DIST += $(service_in_files)
CLEANFILES += $(service_DATA)

%.service: %.service.in Makefile
	$(do_subst) < "$<" > "$@"

# sudo's sudoers files
sudoersdir = $(SUDOERS_D_DIR)
sudoers_in_files = apt-woodchuck.sudoers.in
sudoers_DATA = $(sudoers_in_files:.sudoers.in=.sudoers)

%.sudoers: %.sudoers.in Makefile
	$(do_subst) < "$<" > "$@"

EXTRA_DIST +=  $(sudoers_in_files)
CLEANFILES +=  $(sudoers_DATA)

