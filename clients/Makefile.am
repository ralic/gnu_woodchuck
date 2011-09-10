EXTRA_DIST =
CLEANFILES =

bin_SCRIPTS = vcssync woodchuck-cli
CLEANFILES += $(bin_SCRIPTS)

do_subst = sed -e 's,[@]bindir[@],$(bindir),g' \
	-e 's,[@]PYTHON[@],$(PYTHON),g' \
	-e 's,[@]pythondir[@],$(pythondir),g'

vcssync: vcssync.sh Makefile
	$(do_subst) < "$<" > "$@"
	chmod +x "$@"

woodchuck-cli: woodchuck-cli.sh Makefile
	$(do_subst) < "$<" > "$@"
	chmod +x "$@"

EXTRA_DIST += vcssync.sh woodchuck-cli.sh

python_PYTHON = vcssync.py woodchuck-cli.py

# DBus service files
servicedir = $(DBUS_SERVICES_DIR)
service_in_files = org.woodchuck.vcssync.service.in
service_DATA = $(service_in_files:.service.in=.service)

EXTRA_DIST += $(service_in_files)
CLEANFILES += $(service_DATA)

# Rule to make the service file with bindir expanded
$(service_DATA): $(service_in_files) Makefile
	$(do_subst) < "$<" > "$@"
