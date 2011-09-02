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

import sys
import os
import subprocess
import collections
import gobject
import json
from optparse import OptionParser
import tempfile
import smtplib
import traceback
from email.mime.text import MIMEText
import atexit
import dbus

from dbus.mainloop.glib import DBusGMainLoop
DBusGMainLoop(set_as_default=True)

# Basic configuration parameters.

# The number of synchronization operations to run in parallel.
parallel_transfers = 3

# The default target object freshness (in hours).
default_freshness = 24

# Load and configure the logging facilities.
import logging
logger = logging.getLogger(__name__)

# Save the logging output to a file.  If the file exceeds
# logfile_max_size, rotate it (and remove the old file).
logfile = os.path.expanduser("~/.vcssync.log")
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
#logging_level = logging.DEBUG
logging_level = logging.INFO
logging.basicConfig(
    level=logging_level,
    format=('%(asctime)s (pid: ' + str(os.getpid()) + ') '
            + '%(levelname)-8s %(message)s'),
    filename=logfile,
    filemode='a')

def print_and_log(*args):
    """
    Print the arguments to stdout and send them to the log file.
    """
    logger.warn(*args)
    sys.stdout.write(' '.join(args) + "\n")

logger.info("VCS Sync started (%s)." % (repr(sys.argv)))

# Log uncaught exceptions.
original_excepthook = sys.excepthook

def my_excepthook(exctype, value, tb):
    """Log uncaught exceptions."""
    logger.error(
        "Uncaught exception: %s"
        % (''.join(traceback.format_exception(exctype, value, tb)),))
    original_excepthook(exctype, value, tb)
sys.excepthook = my_excepthook

@atexit.register
def exit_handler():
    logger.info("Exiting.")

# Load woodchuck.
try:
    from pywoodchuck import PyWoodchuck, __file__ as pywoodchuck_file
    import woodchuck
    logging.debug("pywoodchuck loaded successfully (%s, %s)."
                  % (pywoodchuck_file, woodchuck.__file__))
except ImportError, e:
    # If Woodchuck is not found, don't die horribly.
    print_and_log("Loading pywoodchuck failed: %s." % (str(e),))
    class PyWoodchuck(object):
        def __init__(*args):
            pass
        def available(self):
            return False

# The data structure describing a remote repository and what to do
# with it.
#
#  - directory is the root directory of the repository
#  - sync is either 'push' or 'pull'
#  - remote is the repository to push to or from.  If None, the
#      default remote is used.
#  - refs is the set of refs to push or pull.  If None, the default
#      set of references is used.
#  - freshness is how often to perform the action, in days.
Remote = collections.namedtuple(
    'Remote', ['sync', 'directory', 'remote', 'refs', 'freshness'])

def remote_to_id(remote):
    """
    Given a remote, convert it to a Woodchuck id by joining each field
    together and separating them by spaces.

    We assume that none of the components contain a space.
    """
    id = [ remote.sync, remote.directory ]
    if remote.remote:
        id.append(remote.remote)
        if remote.refs:
            id.append(remote.refs)
    else:
        assert remote.refs is None
    return ' '.join(id)

def id_to_remote(id):
    """
    Given an id that was encoded using remote_to_id, convert it to a
    Remote.
    """
    (sync, directory, remote, refs) = (id.split(" ", 3) + [None, None])[:4]
    return Remote(sync, directory, remote, refs, None)

# Class that receives and processes Woodchuck upcalls.
class mywoodchuck(PyWoodchuck):
    def __init__(self, human_readable_name, dbus_service_name,
                 request_feedback):
        # We need to claim the name before we register with Woodchuck.
        #
        # Consider: the reason we started is that Woodchuck might have
        # made an upcall.  The DBus daemon will only queue the message
        # for 25 seconds, after which point it will drop the message
        # on the floor.  Registering with Woodchuck means using DBus.
        # Indeed, it means making a blocking call to Woodchuck.  If
        # Woodchuck is currently running the scheduler (which it
        # likely is if it just made an upcall to us), then we could
        # block long enough that the message is dropped.
        try:
            self.bus_name = dbus.service.BusName(dbus_service_name,
                                                 bus=dbus.SessionBus(),
                                                 do_not_queue=True)
        except dbus.exceptions.NameExistsException, e:
            print_and_log("Already running (Unable to claim %s: %s)."
                          % (dbus_service_name, str(e)))
            sys.exit(1)

        PyWoodchuck.__init__(self, human_readable_name, dbus_service_name,
                             request_feedback)
        if not self.available():
            print_and_log("Woodchuck server not running.")

    # Streams don't have any content to be updated.  Immediately
    # indicate that the update was success, but that we didn't find
    # anything new.
    def stream_update_cb(self, stream):
        remote = id_to_remote(stream.identifier)
        logger.debug("stream update called on %s)" % (str(remote),))

        self[stream.identifier].transferred(
            transferred_up=0, transferred_down=0,
            transfer_time=0, transfer_duration=0, new_objects=0)

    # Objects are updated (unlike streams).  If the object is still in
    # the configuration file, start the update.  Otherwise, unregister
    # the object (or, rather, its stream as each stream has exactly
    # one object).
    def object_transfer_cb(self, stream, object,
                           version, filename, quality):
        logger.debug("object transfer called on %s)"
                     % (stream.identifier,))

        # Reload the configuration.
        load_config()

        remote = id_to_remote(stream.identifier)
        for remote in remotes:
            if remote_to_id(remote) == stream.identifier:
                transfer(remote)
                break
        else:
            # User doesn't want to update this object any more--it's
            # no longer in the config, but we haven't removed it yet.
            logger.debug("object transfer: %s no longer in config."
                         % (stream.identifier,))
            self[stream.identifier][object.identifier].transfer_failed(
                woodchuck.TransferStatus.FailureGone)
            del self[stream.identifier][object.identifier]

# The set of active fetchers/pushers.
Transferer = collections.namedtuple(
    'Transferer', ['process', 'command_line', 'remote', 'output', 'results'])
transferers = []
transferers_queued = []

some_transfer_failed = 0

def idle():
    return not transferers and not transferers_queued

def transfer(remote):
    global transferers
    global transferers_queued

    logger.debug("transfer: transferring %s." % (str(remote),))

    if len(transferers) >= parallel_transfers:
        logger.debug("Maximum transfer parallelism reached (%d).  Enqueuing %s"
                     % (parallel_transfers, str(remote),))
        transferers_queued.append(remote)
        return

    directory = os.path.expanduser(remote.directory)

    # This should ensured by the configuration loader.
    assert remote.sync in ('push', 'pull')

    args = None
    if (# git: Look for a .git directory or the usual files in a bare
        # repository.
        os.path.exists(os.path.join(directory, '.git'))
        or (all(os.path.exists(os.path.join(directory, d))
                for d in ['branches', 'config', 'HEAD', 'hooks',
                          'info', 'objects', 'refs']))):
        args = ['/usr/bin/env', 'git']
    
        if remote.sync == 'push':
            args.append('push')
        elif remote.sync == 'pull':
            args.append('fetch')
    
        if remote.remote is not None:
            args.append(remote.remote)
    
        if remote.refs is not None:
            args.append(remote.refs)
    elif (# Mercurial: Look for a .hg directory
        os.path.exists(os.path.join(directory, '.hg'))):
        args = ['/usr/bin/env', 'hg', remote.sync]
    
        if remote.refs is not None:
            args += ("-r", remote.refs)

        if remote.remote is not None:
            args.append(remote.remote)
    else:
        # Unknown repository.
        print_and_log("%s: %s does not contain a supported repository."
                      % (remote_to_id(remote), directory))
        some_transfer_failed = 1
        return

    logger.debug("Running %s (remote: %s)"
                 % (str(args), str(remote)))

    output = tempfile.TemporaryFile(mode='w+b')

    process = subprocess.Popen(
        args=args,
        cwd=directory,
        stdout=output.fileno(),
        stderr=subprocess.STDOUT)
    transferers.append(Transferer(process, args, remote, output, []))

    poll_start()

poll_id = None

transferers_failed = []

def poll():
    global transferers
    global transferers_failed
    global some_transfer_failed
    global poll_id

    not_finished = []
    for transferer in transferers:
        ret = transferer.process.poll()
        if ret is not None:
            remote = remote_to_id(transferer.remote)

            # Read in any output.
            transferer.output.seek(0)
            output = transferer.output.read()
            transferer.output.close()

            transferer.results.append(ret)
            transferer.results.append(output)

            if ret != 0:
                transferers_failed.append(transferer)
                some_transfer_failed = 1

            print_and_log("%s: %s%s"
                          % (remote,
                             "ok." if ret == 0 else "failed!",
                             ("\n" + output) if ret != 0 else ""))
            try:
                if wc.available():
                    try:
                        stream = wc[remote]
                    except KeyError:
                        stream = wc.stream_register(
                            human_readable_name=remote,
                            stream_identifier=remote,
                            freshness=woodchuck.never_updated)
    
                    if ret == 0:
                        stream.object_transferred(remote)
                    else:
                        stream.object_transfer_failed(
                            remote,
                            woodchuck.TransferStatus.TransientOther)
            except woodchuck.Error, e:
                logger.warn(
                    "Failed to register transfer result with Woodchuck: %s"
                    % (str(e)))
        else:
            not_finished.append(transferer)

    transferers = not_finished

    if len(transferers) < parallel_transfers and transferers_queued:
        transfer(transferers_queued.pop(0))

    if idle():
        if transferers_failed:
            if options.daemon:
                message = """
The following errors occured while synchronizing your repositories:

"""
                for transferer in transferers_failed:
                    message += (
                        "Transfering %s failed.\n  %s -> exit code %d.\n%s\n"
                        % (remote_to_id(transferer.remote),
                           ' '.join("'" + a + "'"
                                    for a in transferer.command_line),
                           transferer.results[0],
                           (("  " + l) for l in transferer.results[1])))

                msg = MIMEText(message)
                msg['Subject'] = 'VCSSync: Synchronization failed.'
                email = os.environ['USER'] + '@localhost'
                msg['From'] = '"VCSSync Daemon" <%s>' % email
                msg['To'] = '<%s>' % email

                s = smtplib.SMTP('localhost')
                s.sendmail(email, [email], msg.as_string())
                s.quit()

            transferers_failed[:] = []
        poll_id = None
        inactive_timeout_start()
        return False
    else:
        return True

def poll_start():
    global poll_id
    if not poll_id:
        poll_id = gobject.timeout_add(2000, poll)

# If running in daemon mode and there is no activity for a while (in
# milliseconds), quit.
inactive_duration = 5 * 60 * 1000

inactive_timeout_id = None
def inactive_timeout():
    if idle():
        logger.debug("Inactive, quitting.")
        mainloop.quit()

    inactive_timeout_remove()
    return False

def inactive_timeout_remove():
    global inactive_timeout_id
    if inactive_timeout_id:
        gobject.source_remove(inactive_timeout_id)
        inactive_timeout_id = None

def inactive_timeout_start():
    global inactive_timeout_id

    inactive_timeout_remove()

    if not options.daemon:
        inactive_timeout()
    else:
        logger.debug("Started inactivity timer.")
        inactive_timeout_id \
            = gobject.timeout_add(inactive_duration, inactive_timeout)

# Command line options.
parser = OptionParser()
parser.add_option("-d", "--daemon", action="store_true", default=False,
                  help="don't automatically synchronize all files.")
parser.add_option("-r", "--reload-config", action="store_true", default=False,
                  help="just read the configuration file, then quit.")
(options, args) = parser.parse_args()

if args:
    print "Unknown arguments: %s" % (str(args))
    parser.print_help()
    sys.exit(1)

# Config file parsing.
remotes = []

config_file = "~/.vcssync"
config_file_expanded = os.path.expanduser(config_file)
config_file_loaded = None

config_file_example = """\
// This file is a json file.
//   
//  vcssync expects a single data structure, a hash mapping repository
//  directories to an array of hashes.  The array is a list of
//  synchronization actions to perform on the repository.  Each inner
//  hash contains of up to four keys:
// 
//   'sync': The direction to synchronize: either 'pull' or 'push',
//     (default: 'pull').
//   'remote': The remote to sync with (default: the VCS's default,
//     typically 'origin').
//   'refs': The set of references to synchronize (default: all
//     references).
//   'freshness': How out of data to allow the copy to become, in hours
//     (default: 24).  In other words, approximately how often to perform
//     the action.
{
    "~/src/some-project": [
	{"sync": "pull"},
	{"sync": "pull", "remote": "gitorious-alice", freshness:48},
	{"sync": "pull", "remote": "gitorious-bob", freshness:48},
        // Push to the backup server approximately every two hours.
	{"sync": "push", "remote": "backup-server", freshness:2}
    ],
    "~/src/other-project": [
	{"sync": "pull"},
	{"sync": "push", "remote": "backup-server", freshness:2}
    ]
}
"""
def load_config():
    global remotes
    global config_file_loaded

    if not os.path.isfile(config_file_expanded):
        print_and_log("Configuration file (%s) does not exist."
                      % (config_file,))

        with open(config_file_expanded, "w") as fhandle:
            print >>fhandle, config_file_example,

        print_and_log("Example configuration file created.")

        sys.exit(1)

    mtime = os.stat(config_file_expanded).st_mtime
    if config_file_loaded is not None:
        if config_file_loaded == mtime:
            logger.debug("Config file unchanged, not reloading.")
            return
        else:
            logger.debug("Config file changed, reloading.")

    config_file_loaded = mtime

    remotes = []
    config = None
    try:
        raw_data = ""
        data = ""
        with open(os.path.expanduser(config_file)) as fhandle:
            for line in fhandle:
                raw_data += line

                line = line.strip()
                if line[:2] == "//":
                    # Strip comments, which the json parser does not
                    # support, but, preserve lines numbers to improve
                    # error messages.
                    data += "\n"
                    continue
                else:
                    data += line + "\n"

            if raw_data == config_file_example:
                print_and_log("vcssync unconfigured.  Please edit %s."
                              % (config_file,))
                sys.exit(1)

            config = json.loads(data)
    except ValueError, e:
        print_and_log("Error parsing %s: %s" % (config_file, str(e)))
        sys.exit(1)
    except OSError, e:
        print_and_log("Error opening %s: %s" % (config_file, str(e)))
        sys.exit(1)
    
    error = False
    remotes = []
    for repository, lines in config.items():
        for line in lines:
            try:
                remote = line.get('remote', None)
    
                sync = line.get('sync', 'pull')
                if sync not in ('pull', 'push'):
                    print_and_log(("Unsupported sync value for %s/%s: %s"
                                   + " (either push or pull)")
                                  % (repository, remote, sync))
    
                refs = line.get('refs', None)
    
                freshness = line.get('freshness', default_freshness)

                remotes.append(Remote(sync, repository, remote, refs,
                                      freshness))
            except AttributeError:
                error = True
                print_and_log("Error processing %s's stanza: %s"
                              % (repository, str(line)))

    if error:
        print_and_log("Error parsing %s." % (config_file,))
        sys.exit(1)
    
    if not remotes:
        print_and_log("No repositories defined in %s." % (config_file,))
        sys.exit(0)
    
    for i, remote in enumerate(remotes):
        logger.debug("%d: Configured action: %s" % (i, str(remote)))

load_config()

# Woodchuck synchronization.
wc = None

def register(remote):
    """
    Register the remote.  Do not die if the remote is already
    registered.

    Returns True if something was registered or updated.  False,
    otherwise.
    """
    id = remote_to_id(remote)
    try:
        wc.stream_register(human_readable_name=id,
                           stream_identifier=id,
                           freshness=woodchuck.never_updated)
    except woodchuck.ObjectExistsError:
        pass

    freshness = remote.freshness * 24 * 60 * 60
    try:
        wc[id].object_register(human_readable_name=id,
                               object_identifier=id,
                               transfer_frequency=freshness)
        return True
    except woodchuck.ObjectExistsError:
        if wc[id][id].transfer_frequency != freshness:
            wc[id][id].transfer_frequency = freshness
            return True

    return False

def main():
    # Only request feedback if we are running as a daemon.
    global wc
    wc = mywoodchuck("VCS Sync", "org.woodchuck.vcssync",
                     request_feedback=options.daemon)

    if options.daemon or options.reload_config:
        # Only bother synchronizing the configuration and the Woodchuck
        # configuration if we are running as a daemon.
        if wc.available():
            # Register any previously unknown streams.
            streams = wc.streams_list()
            stream_ids = [s.identifier for s in streams]
        
            logger.debug("Registered streams: %s" % (str(stream_ids),))
            logger.debug("Configured streams: %s" % (str(remotes),))
    
            changes = 0
            for remote in remotes:
                id = remote_to_id(remote)
                if id in stream_ids:
                    stream_ids.remove(id)
    
                if register(remote):
                    changes += 1

            # Unregister streams that the user is no longer interested in
            # (i.e., removed from the configuration file.
            for id in stream_ids:
                logger.info("Unregistering %s" % id)
                del wc[id]
    
        if (options.reload_config or changes or len(stream_ids)):
            print_and_log("%d synchronization actions updated, %d pruned."
                          % (changes, len(stream_ids)))

        if options.reload_config:
            sys.exit(0)
    
        inactive_timeout_start()
    else:
        # We are not running in daemon mode.  Update everything.
        for remote in remotes:
            transfer(remote)

gobject.idle_add(main)
mainloop = gobject.MainLoop()
mainloop.run()
if some_transfer_failed:
    sys.exit(1)
