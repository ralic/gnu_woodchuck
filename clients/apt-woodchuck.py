#! /usr/bin/env python

# Copyright 2011 Neal H. Walfield <neal@walfield.org>
#
# This file is part of Woodchuck.
#
# Woodchuck is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# Woodchuck is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
# License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see
# <http://www.gnu.org/licenses/>.

from __future__ import with_statement

import sys
import os
import errno
import glob
import time
import traceback
import gconf
import dbus
import gobject
import subprocess
import apt
import threading
import curses.ascii
from optparse import OptionParser
from pywoodchuck import PyWoodchuck
import woodchuck

from dbus.mainloop.glib import DBusGMainLoop
DBusGMainLoop(set_as_default=True)

# Load and configure the logging facilities.
import logging
logger = logging.getLogger(__name__)

# Save the logging output to a file.  If the file exceeds
# logfile_max_size, rotate it (and remove the old file).
logfile = os.path.expanduser("~/.apt-woodchuck.log")
logfile_max_size = 1024 * 1024
try:
    logfile_size = os.stat(logfile).st_size
except OSError:
    # Most likely the file does not exist.
    logfile_size = 0

if logfile_size > logfile_max_size:
    try:
        old_logfile = logfile + ".old"
        os.rename(logfile, old_logfile)
    except OSError, e:
        print "Renaming %s to %s: %s" % (logfile, old_logfile, str(e))

# The desired logging level.
logging_level = logging.DEBUG
#logging_level = logging.INFO
logging.basicConfig(
    level=logging_level,
    format=('%(asctime)s (pid: ' + str(os.getpid()) + '; %(threadName)s) '
            + '%(levelname)-8s %(message)s'),
    filename=logfile,
    filemode='a')

def print_and_log(*args):
    """
    Print the arguments to stdout and send them to the log file.
    """
    logger.warn(*args)
    sys.stdout.write(' '.join(args) + "\n")

logger.info("APT Woodchuck started (%s)." % (repr(sys.argv)))

# Log uncaught exceptions.
original_excepthook = sys.excepthook

def my_excepthook(exctype, value, tb):
    """Log uncaught exceptions."""
    print(
        "Uncaught exception: %s"
        % (''.join(traceback.format_exception(exctype, value, tb)),))
    original_excepthook(exctype, value, tb)
sys.excepthook = my_excepthook

def redirect(thing):
    filename = os.path.join(os.environ['HOME'], '.apt-woodchuck.' + thing)
    try:
        with open(filename, "r") as fhandle:
            contents = fhandle.read()
    except IOError, e:
        if e.errno in (errno.ENOENT,):
            fhandle = None
            contents = ""
        else:
            logging.error("Reading %s: %s" % (filename, str(e)))
            raise

    logging.error("std%s of last run: %s" % (thing, contents))

    if fhandle is not None:
        os.remove(filename)

    print "Redirecting std%s to %s" % (thing, filename)
    return open(filename, "w", 0)

sys.stderr = redirect('err')
sys.stdout = redirect('out')

def maemo():
    """Return whether we are running on Maemo."""
    # On Mameo, this file is included in the dpkg package.
    return os.path.isfile('/etc/dpkg/origins/maemo')

def ham_check_interval_update(verbose=False):
    """Configure Maemo to never do an update on its own."""
    client = gconf.client_get_default()

    check_interval = client.get_int(
        '/apps/hildon/update-notifier/check_interval')
    check_interval_never = (1 << 31) - 1
    if check_interval != check_interval_never:
        print_and_log("Updating check interval from %d to %d seconds"
                      % (check_interval, check_interval_never))
        client.set_int('/apps/hildon/update-notifier/check_interval',
                       check_interval_never)
    elif verbose:
        print_and_log("HAM update check interval set to %d seconds."
                      % check_interval)

class Job(threading.Thread):
    """This class implements a simple job manager: Jobs can be queued
    from the main thread and they are executed serially, FIFO."""

    # The job that is currently executing, or, None, if none.
    running = None
    # List of queued jobs.
    jobs = []

    # A lock to protect the above two variables.
    lock = threading.Lock()

    def __init__(self, key, func, func_args=None, **kwargs):
        """Create and queue a job.  kwargs can include the special
        arguments on_start and on_finish, which are methods (which are
        passed no arguments) that will be invoked before the job is
        started or after it is finished."""
        threading.Thread.__init__(self)

        self.key = key

        if 'on_start' in kwargs:
            self.on_start = kwargs['on_start']
            del kwargs['on_start']
        else:
            self.on_start = None
        if 'on_finish' in kwargs:
            self.on_finish = kwargs['on_finish']
            del kwargs['on_finish']
        else:
            self.on_finish = None
            
        self.func = func
        self.args = func_args if func_args is not None else []
        self.kwargs = kwargs

        with self.lock:
            jobs = self.jobs[:]
            if self.__class__.running is not None:
                jobs.append(self.__class__.running)
            for j in jobs:
                if j.key == key:
                    logger.debug("Job %s already queued, ignoring." % key)
                    break
            else:
                logger.debug("Job %s enqueued." % key)
                self.jobs.append(self)

        Job.scheduler()

    def run(self):
        with self.lock:
            assert self.__class__.running == self

        try:
            logger.debug("Running job %s" % self.key)
            if self.on_start is not None:
                self.on_start()

            self.func(*self.args, **self.kwargs)

            if self.on_finish is not None:
                self.on_finish()
        except Exception, e:
            logger.exception("Executing %s: %s" % (self.key, str(e)))
        finally:
            with self.lock:
                assert self.__class__.running is self
                self.__class__.running = None

            # Run the next job.
            Job.scheduler()

    @classmethod
    def scheduler(cls):
        """Try and execute a job."""
        with cls.lock:
            if cls.running is not None:
                return

            try:
                job = cls.jobs.pop(0)
            except IndexError:
                return

            cls.running = job
        job.start()

APT_CACHE_DIRECTORY = "/var/cache/apt/archives"

dbus_service_name = "org.woodchuck.apt-woodchuck"

# Class that receives and processes Woodchuck upcalls.
class AptWoodchuck(PyWoodchuck):
    packages_stream_identifier = 'packages'

    def __init__(self, daemon):
        # We need to do three things:
        # - Claim our DBus name.
        # - Connect to Woodchuck
        # - Make sure that Woodchuck knows about all of our streams
        #   and objects.  This really only needs to be done the
        #   first time we are run.  If daemon is False, then after we
        #   register, we quit.

        # Miscellaneous configuration.

        # Whether an update is in progress.
        self.ham_update_check_progress_id = 0


        if daemon:
            # Claim our DBus service name.
            #
            # We do this as soon as possible to avoid missing messges.
            # (DBus queues messages for 25 seconds.  Sometimes, that is
            # just not enough.)
            try:
                self.bus_name = dbus.service.BusName(dbus_service_name,
                                                     bus=dbus.SessionBus(),
                                                     do_not_queue=True)
            except dbus.exceptions.NameExistsException, e:
                print_and_log("Already running (Unable to claim %s: %s)."
                              % (dbus_service_name, str(e)))
                sys.exit(1)


        # Connect to Woodchuck.
        #
        # The human readable name will be shown to the user.  Be
        # careful what you choose: you can't easily change it;
        # PyWoodchuck uses both the human readable name AND the dbus
        # service name to identify itself to Woodchuck.
        PyWoodchuck.__init__(
            self, human_readable_name="Application Update Manager",
            dbus_service_name=dbus_service_name,
            request_feedback=daemon)

        # Check if Woodchuck is really available.  If not, bail.
        if not self.available():
            print_and_log("Unable to contact Woodchuck server.")
            sys.exit(1)


        # Register our streams and objects with Woodchuck.
        #
        # This program uses one stream: the 'package' stream.
        # Updating the stream means doing an 'apt-get update'.  We
        # only register packages for which an update is available:
        # these are bits that the user is very likely interested in;
        # the expected utility of prefetching packages that are not
        # installed is low, and we don't have enough disk space to
        # mirror the whole archive anyway.

        # Be default, we want to do an apt-get update approximately
        # every day.
        freshness=24 * 60 * 60
        try:
            self.stream_register(
                stream_identifier=self.packages_stream_identifier,
                human_readable_name='Packages',
                freshness=freshness)
        except woodchuck.ObjectExistsError:
            self[self.packages_stream_identifier].freshness = freshness

        if not daemon:
            print_and_log("Registered Woodchuck callbacks.")
            sys.exit(0)

        # Register packages with Woodchuck for which an update is
        # available.
        self.register_available_updates()

    def stream_update_cb(self, stream):
        logger.debug("stream update called on %s" % (str(stream.identifier),))

        if stream.identifier != self.packages_stream_identifier:
            logger.info("Unknown stream: %s (%s)"
                        % (stream.human_readable_name, stream.identifier))
            try:
                del self[stream.identifier]
            except woodchuck.Error, e:
                logger.info("Removing unknown stream %s: %s"
                            % (stream.identifier, str(e)))
            return

        # Start an update.
        Job(key='stream:%s' % (str(stream.identifier),),
            on_start=self.inactive_timeout_remove,
            on_finish=self.inactive_timeout_start,
            func=self.apt_get_update)

    def object_transfer_cb(self, stream, object,
                           version, filename, quality):
        logger.debug("object transfer called on stream %s, object %s"
                     % (stream.identifier, object.identifier))

        if stream.identifier != self.packages_stream_identifier:
            logger.info("Unknown stream %s" % (stream.identifier))
            try:
                del self[stream.identifier]
            except woodchuck.Error, e:
                logger.info("Removing unknown stream %s: %s"
                            % (stream.identifier, str(e)))
            return

        # Prefetch the specified object.
        try:
            Job(key='object:%s' % (str(object.identifier),),
                on_start=self.inactive_timeout_remove,
                on_finish=self.inactive_timeout_start,
                func=self.apt_get_download, func_args=[object.identifier])
        except Exception, e:
            logger.exception("Creating Job for object transfer %s: %s"
                             % (object.identifier, str(e),))

    # APT package cache management.
    def apt_cache_check_validity(self, force_reload=False):
        """Reload the apt package cache if necessary."""
        if not hasattr(self, '_apt_cache'):
            self._apt_cache = apt.Cache()
        elif (self._apt_cache_last_refreshed + 30 > time.time()
            or force_reload):
            self._apt_cache.open(None)
        else:
            return

        self._apt_cache_last_refreshed = time.time()

    def apt_cache_invalidate(self):
        """Invalidate the apt package cache forcing a reload the next
        time it is needed."""
        self._apt_cache_last_refreshed = 0

    @property
    def apt_cache(self):
        """Return an apt package cache object."""
        self.apt_cache_check_validity()
        return self._apt_cache

    def apt_cache_filename(self, package_version,
                           version_string=None, arch_string=None):
        """Return the filename that would be used to save a version of
        a package."""

        # Look at
        # apt/apt-pkg/acquire-item.cc:pkgAcqArchive::pkgAcqArchive and
        # apt/apt-pkg/contrib/strutl.cc:QuoteString to see how the
        # filename is created.
        def escape(s, more_bad=''):
            escaped = []
            have_one = False
            for c in s:
                if (c in ('_:%' + more_bad)
                    or not curses.ascii.isprint(c)
                    or ord(c) <= 0x20 or ord(c) >= 0x7F):
                    have_one = True
                    escaped.append("%%%02x" % ord(c))
                else:
                    escaped.append(c)

            if not have_one:
                return s

            return ''.join(escaped)

        try:
            package_name = package_version.package.name

            if version_string is None:
                version_string = package_version.version

            if arch_string is None:
                arch_string = package_version.architecture
        except AttributeError:
            # Assume package_version is a string and that
            # version_string and arch_string are provided.
            package_name = package_version

        s = (escape(package_name)
             + '_' + escape(version_string)
             + '_' + escape(arch_string, more_bad='.')
             + '.deb')

        logger.debug(
            "Cache file for (%s, %s, %s): %s"
            % (package_name, str(version_string), str(arch_string), s))

        return os.path.join(APT_CACHE_DIRECTORY, s)

    def apt_get_update(self):
        """Perform an update."""
        logger.info("Running apt-get update")

        if self.ham_update_check_progress_id:
            logger.info("apt-get update already in progress.")
            return

        transfer_start = time.time()

        success = False
        if maemo():
            # On Maemo, we don't directly call apt-get update as the
            # hildon package manager does some other magic, such as
            # updating status files in
            # /home/user/.hildon-application-manager.
            process = subprocess.Popen(
                args=["/usr/libexec/apt-worker", "check-for-updates"],
                close_fds=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT)

            # Read stdout (and stderr).
            output = process.stdout.read()
            logger.info("apt-worker(%d): %s" % (process.pid, output))

            # Process closed stdout.  Wait for it to finish executing
            # and get the return code.
            exit_code = process.wait()
            logger.info("apt-worker: exit code: %d" % (exit_code,))

            if exit_code == 0:
                success = True
        else:
            # On "generic" Debian, we use apt-get update.
            try:
                success = self.apt_cache.update()
            except apt.cache.LockFailedException, e:
                logger.info("apt_get_update: Failed to obtain lock: %s"
                            % str(e))
            except Exception, e:
                logger.exception("apt_get_update: %s" % repr(e))

            logger.info("apt-get update returned: %s" % str(success))

        transfer_duration = time.time() - transfer_start
        logger.debug("apt-get update took %d seconds" % (transfer_duration,))

        # Force the APT cache database to be reloaded in this thread
        # (rather than blocking the main thread).
        self.apt_cache_check_validity(force_reload=True)

        # We can only execute Woodchuck calls in the main thread as
        # DBus is not thread safe.  gobject.idle_add is thread safe.
        # We use it to register a callback in the main thread.
        def report_to_woodchuck():
            if success:
                new_updates = self.register_available_updates()

                self[self.packages_stream_identifier].updated(
                    # Would be nice to get these values:
                    # transferred_up=..., transferred_down=...,
                    transfer_time=transfer_start,
                    transfer_duration=transfer_duration,
                    new_objects=new_updates,
                    objects_inline=0)
            else:
                self[self.packages_stream_identifier].update_failed(
                    reason=woodchuck.TransferStatus.TransientOther
                    #transferred_up=0, transferred_down=0
                    )

        gobject.idle_add(report_to_woodchuck)

    def register_available_updates(self):
        """Determine the set of installed packages that can be updated
        and register them with Woodchuck."""
        logger.info("Synchronizing available updates with Woodchuck.")

        new_updates = 0

        # The packages registered with Woodchuck.
        registered_packages = self[self.packages_stream_identifier].keys()
        logger.debug("Packages registered with Woodchuck: %s"
                     % (str(registered_packages),))

        for pkg in self.apt_cache:
            version = pkg.candidate

            if pkg.isUpgradable:
                # The package is upgradable.  Either: we haven't yet
                # registered the package with Woodchuck, or we have.
                # In the latter case, we still might have to do
                # something if there is a newer version available.
                if pkg.name not in registered_packages:
                    # The package is not yet registered with
                    # Woodchuck.  Do so.
                    logger.info("Registering %s package update" % (pkg.name,))
                    try:
                        self[self.packages_stream_identifier].object_register(
                            object_identifier=pkg.name,
                            human_readable_name=pkg.name,
                            expected_size=version.size)
                        new_updates += 1
                    except woodchuck.Error, e:
                        logger.error("Registering package %s: %s"
                                     % (pkg.name, str(e)))
                else:
                    # The package is already registered.
                    #
                    # There are a few scenarios:
                    #
                    # - The package has been downloaded but not yet
                    #   updated.  There is nothing to do, but we
                    #   should be careful to not trigger it to be
                    #   downloaded again (e.g., by claiming there is
                    #   an update available by setting NeedUpdate to
                    #   true.)
                    #
                    # - An old version of the package has been
                    #   downloaded.  We need to tell Woodchuck to
                    #   download the object again so that we have the
                    #   latest version.
                    #
                    # - The package has not been downloaded yet.
                    #   There is nothing to do (setting NeedUpdate in
                    #   this case doesn't hurt).  Eventually,
                    #   Woodchuck will tell us to download the object.

                    # Check if the version we want is downloaded.
                    filename = self.apt_cache_filename(version)
                    if os.path.isfile(filename):
                        # The latest version is available.  There is
                        # nothing to do.
                        pass
                    else:
                        logger.debug("Setting '%s'.NeedUpdate" % (pkg.name,))
                        try:
                            self[self.packages_stream_identifier][pkg.name] \
                                .need_update = True
                        except (KeyError, woodchuck.Error), e:
                            logger.error("Setting %s.NeedUpdate: %s"
                                         % (pkg.name, str(e)))

                    registered_packages.remove(pkg.name)

        logger.debug("Unregistering objects %s" % (registered_packages,))
        for package_name in registered_packages:
            # The package is registered, but is no longer upgradable.
            # This is likely because the user installed the updated
            # (although it may be just have become uninstallable).
            # Remove any cache files and unregister the object.
            logger.debug("Unregistering object %s" % (package_name,))

            for filename in glob.glob(
                self.apt_cache_filename(str(package_name), '*', '*')):
                try:
                    logger.debug("Removing %s" % (filename,))
                    os.remove(filename)
                except OSError, e:
                    logger.error("Removing %s: %s" % (filename, str(e)))

            try:
                del self[self.packages_stream_identifier][package_name]
            except (KeyError, woodchuck.Error), e:
                logger.info("Unregistering object %s: %s"
                            % (package_name, str(e)))

        return new_updates

    def apt_get_download(self, package_name):
        """Download the specified package."""
        try:
            pkg = self.apt_cache[package_name]
        except KeyError:
            logger.error("Package %s unknown." % (package_name,))
            return

        version = pkg.candidate

        transfer_time = time.time()

        logger.info("Downloading package %s" % (package_name,))
        try:
            filename = version.fetch_binary(APT_CACHE_DIRECTORY)
            logger.debug("Downloaded %s -> %s" % (package_name, filename))
            if filename is None:
                # If the binary is already downloaded, filename is set
                # to None.  If this really looks like the case,
                # succeed.
                filenames \
                    = glob.glob(self.apt_cache_filename(pkg.name, '*', '*'))
                if filenames:
                    filename = filenames[0]
                    ok = True
                else:
                    logger.error("Downloading or saving package %s failed."
                                 % package_name)
                    ok = False
            else:
                ok = True
        except Exception, e:
            logger.exception("Downloading %s: %s" % (package_name, str(e)))
            ok = False

        transfer_duration = time.time() - transfer_time

        def register_result():
            try:
                 if ok:
                     self[self.packages_stream_identifier].object_transferred(
                         object_identifier=package_name,
                         # Would be nice to get these values:
                         # transferred_up=0, transferred_down=0,
                         transfer_time=transfer_time,
                         transfer_duration=transfer_duration,
                         object_size=version.size,
                         files=[(filename,
                                 True, # The file is not shared.
                                 woodchuck.DeletionPolicy.DeleteWithoutConsultation),
                                ])
                 else:
                     self[self.packages_stream_identifier].object_transfer_failed(
                         object_identifier=package_name,
                         # Assume the failure is transient.
                         reason=woodchuck.TransferStatus.TransientOther,
                         transferred_up=0, transferred_down=0)
            except Exception, e:
                logger.exception("Reporting transfer of %s: %s"
                                 % (package_name, str(e)))
            except woodchuck.Error, e:
                logger.exception("Reporting transfer of %s: %s"
                                 % (package_name, str(e)))
        gobject.idle_add(register_result)

    # Inactivity manager: if there is no activity for a while (in
    # milliseconds), quit.
    inactive_duration = 5 * 60 * 1000

    inactive_timeout_id = None
    def inactive_timeout(self):
        """The timer fired.  Quit."""
        print_and_log("Inactive, quitting.")
        mainloop.quit()
        self.inactive_timeout_remove()

    def inactive_timeout_remove(self):
        """Remove any timer, e.g., because there was some activity."""
        if self.inactive_timeout_id:
            gobject.source_remove(self.inactive_timeout_id)
            self.inactive_timeout_id = None
    
    def inactive_timeout_start(self):
        """Start (or, it if is already running, reset) the timer."""
        self.inactive_timeout_remove()
        self.inactive_timeout_id \
            = gobject.timeout_add(self.inactive_duration, self.inactive_timeout)

def main():
    # Command line options.
    description="""\
APT Woodchuck, a package updater for Woodchuck.

APT Woodchuck updates APT's list of packages and downloads package
updates when a good Internet connection is available as determined by
Woodchuck.

When run as a daemon (--daemon), waits for instructions from the
Woodchuck server.  APT Woodchuck automatically quits if it has nothing
to do a few minutes.

When run without any options, APT Woodchuck registers with the
Woodchuck server and disables the Hildon Application Manager's
automatic updates.  NOTE: APT Woodchuck must be run once in order to
make Woodchuck aware of APT Woodchuck.

Messages are logged to $HOME/.apt-woodchuck.log.
"""
    parser = OptionParser(description=description)
    # By default, the description is refilled without respecting
    # paragraph boundaries.  Override this.
    parser.format_description = lambda self: description
    parser.add_option("-d", "--daemon", action="store_true", default=False,
                      help="Run in daemon mode.")

    (options, args) = parser.parse_args()
    if args:
        print "Unknown arguments: %s" % (str(args))
        parser.print_help()
        sys.exit(1)

    if maemo():
        ham_check_interval_update(verbose=not options.daemon)

    wc = AptWoodchuck(options.daemon)

gobject.threads_init()
gobject.idle_add(main)
mainloop = gobject.MainLoop()
mainloop.run()
