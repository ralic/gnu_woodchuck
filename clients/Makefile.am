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

