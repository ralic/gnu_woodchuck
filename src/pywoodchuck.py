#! /bin/python

# pywoodchuck.py - A high-level woodchuck module for Python.
# Copyright (C) 2011 Neal H. Walfield <neal@walfield.org>
#
# Woodchuck is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 2, or (at
# your option) any later version.
#
# Woodchuck is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.

import woodchuck

"""The stream will never be updated."""
NEVER_UPDATED=2 ** 32 - 1

class PyWoodchuck:
    """

    Before instantiating this class (and, indeed, before using DBus
    function at all), you need to call either:

      from dbus.mainloop.glib import DBusGMainLoop
      DBusGMainLoop(set_as_default=True)

    or
      
      from dbus.mainloop.qt import DBusQtMainLoop
      DBusQtMainLoop(set_as_default=True)
    """

    class _upcalls(woodchuck.Upcalls):
        """Handles upcalls from the woodchuck server and translates
        them to class method calls."""
        def __init__(self, pywoodchuck):
            self.pywoodchuck = pywoodchuck
            woodchuck.Upcalls.__init__(self, "/org/woodchuck")
            self.pywoodchuck.manager.feedback_subscribe (False)

        def object_downloaded_cb (self, manager_UUID, manager_cookie,
                                  stream_UUID, stream_cookie,
                                  object_UUID, object_cookie,
                                  status, instance, version, filename, size,
                                  trigger_target, trigger_fired):
            """org.woodchuck.upcalls.ObjectDownloaded"""
            try:
                stream = self.pywoodchuck._stream_lookup \
                    (stream_cookie, stream_UUID)
                object = self.pywoodchuck._object_lookup \
                    (stream_cookie, object_cookie, object_UUID)
            except woodchuck.NoSuchObject as exception:
                print "Woodchuck invoked " \
                    "org.woodchuck.upcall.ObjectDownloaded", \
                    "for non-existant object: ", str (exception)
                return False

            self.pywoodchuck.object_downloaded_cb \
                (stream_cookie, object_cookie,
                 status, instance, version, filename, size,
                 trigger_target, trigger_fired)

        def stream_update_cb(self, manager_UUID, manager_cookie,
                             stream_UUID, stream_cookie):
            """org.woodchuck.upcalls.StreamUpdate."""
            try:
                stream = self.pywoodchuck._stream_lookup \
                    (stream_cookie, stream_UUID)
            except woodchuck.NoSuchObject as exception:
                print "Woodchuck invoked " \
                    "org.woodchuck.upcall.ObjectDownloaded", \
                    "for non-existant object: ", str (exception)
                return False

            self.pywoodchuck.stream_update_cb (stream_cookie)

        def object_download_cb (self, manager_UUID, manager_cookie,
                                stream_UUID, stream_cookie,
                                object_UUID, object_cookie,
                                version, filename, quality):
            """org.woodchuck.upcalls.ObjectDownload"""
            try:
                stream = self.pywoodchuck._stream_lookup \
                    (stream_cookie, stream_UUID)
                object = self.pywoodchuck._object_lookup \
                    (stream_cookie, object_cookie, object_UUID)
            except woodchuck.NoSuchObject as exception:
                print "Woodchuck invoked " \
                    "org.woodchuck.upcall.ObjectDownload", \
                    "for non-existant object: ", str (exception)
                return False

            self.pywoodchuck.object_download_cb (stream_cookie, object_cookie,
                                                 version, filename, quality)

        def object_delete_files_cb (self, manager_UUID, manager_cookie,
                                    stream_UUID, stream_cookie,
                                    object_UUID, object_cookie):
            """org.woodchuck.upcalls.ObjectDownload"""
            try:
                stream = self.pywoodchuck._stream_lookup \
                    (stream_cookie, stream_UUID)
                object = self.pywoodchuck._object_lookup \
                    (stream_cookie, object_cookie, object_UUID)
            except woodchuck.NoSuchObject as exception:
                print "Woodchuck invoked " \
                    "org.woodchuck.upcall.ObjectDeleteFiles", \
                    "for non-existant object: ", str (exception)
                return False

            self.pywoodchuck.object_delete_files_cb (stream_cookie,
                                                     object_cookie)

    def __init__(self, human_readable_name, dbus_service_name,
                 request_feedback=True):
        try:
            # Try to register a new manager.
            self.manager = woodchuck.manager_register \
                (only_if_cookie_unique=True,
                 human_readable_name=human_readable_name,
                 cookie=dbus_service_name,
                 dbus_service_name=dbus_service_name)
        except woodchuck.ObjectExistsError:
            # Whoops, it failed.  Look up the manager(s) with the
            # cookie.
            self.manager = None
            for m in woodchuck.lookup_manager_by_cookie (dbus_service_name,
                                                         False):
                if m.human_readable_name == human_readable_name:
                    # Human readable name also matches.  Looks good.
                    if self.manager is None:
                        self.manager = m
                    else:
                        raise woodchuck.ObjectExistsError \
                            ("Multiple objects with dbus_service_name",
                             dbus_service_name, "and human_readable_name",
                             human_readable_name, "exist.")
            if self.manager is None:
                raise woodchuck.ObjectExistsError \
                    ("Multiple objects with dbus_service_name",
                     dbus_service_name, "exist, but not have",
                     "human_readable_name", human_readable_name, ".")

        # dict from a stream's cookie to a <stream UUID, object dict>
        # tuple.  The object dict is a dict from an object's cookie to
        # its UUID.
        self.streams = {}

        if request_feedback:
            if (self.object_downloaded_cb == PyWoodchuck.object_downloaded_cb
                and self.stream_update_cb == PyWoodchuck.stream_update_cb
                and self.object_download_cb == PyWoodchuck.object_download_cb
                and (self.object_delete_files_cb
                     == PyWoodchuck.object_delete_files_cb)):
                # None of the call backs were overriden.  There is no
                # need to subscribe.
                pass
            else:
                self.upcalls = self._upcalls (self)

    def stream_register(self, stream_identifier,
                        human_readable_name, freshness=0):
        """Register a stream.

        STREAM_IDENTIFIER is a free-form string, which is
        uninterpreted by the server and provided on upcalls.  It must
        uniquely identify the stream within the application.  It can
        be an application specific key, e.g., the URL of an RSS feed.

        HUMAN_READABLE_NAME is a string that will be shown to the user
        and should unambiguously identify the stream in the context of
        the application.  For the "Foo Email Client," if there is only
        one Inbox, "Inbox" is sufficient for identifing the inbox
        stream; "Foo EMail Client: Inbox" is unnecessarily long.

        FRESHNESS is approximately how often the stream should be
        updated, in seconds.  This value is interpretted as a hint.
        Woodchuck interprets 0 as meaning there are no freshness
        requirements and it is completely free to choose when to
        update the stream.  A value of NEVER_UPDATED is interpretted as
        meaning that the stream is never updated."""
        self.manager.stream_register \
            (only_if_cookie_unique=True,
             human_readable_name=human_readable_name,
             cookie=stream_identifier,
             freshness=freshness)

    def streams_list(self):
        return self.manager.list_streams ()

    def _stream_lookup(self, stream_identifier, UUID=None):
        class _Stream:
            def __init__(self, stream):
                self.llstream = stream
                self.objects = {}

            def __repr__(self):
                return "Stream wrapper of " % self.llstream.human_readable_name

        if stream_identifier not in self.streams:
            if UUID is not None:
                self.streams[stream_identifier] \
                    = _Stream(woodchuck.Stream (UUID=UUID,
                                                cookie=stream_identifier))
            else:
                streams = self.manager.lookup_stream_by_cookie \
                    (stream_identifier)
                if len (streams) > 1:
                    raise woodchuck.InternalError \
                        ("Internal inconsistency, this is likely a programmer "
                         + "error: multiple streams with identifier "
                         + stream_identifier)
                if len (streams) == 0:
                    raise woodchuck.NoSuchObject ("No streams with identifier "
                                                  + stream_identifier);
                self.streams[stream_identifier] = _Stream(streams[0])
        return self.streams[stream_identifier]

    def _object_lookup(self, stream_identifier,
                       object_identifier, object_UUID=None):
        stream = self._stream_lookup (stream_identifier)
        if object_identifier not in stream.objects:
            if object_UUID is not None:
                stream.objects[object_identifier] \
                    = woodchuck.Object (UUID=object_UUID,
                                        cookie=object_identifier)
            else:
                objects = stream.llstream.lookup_object_by_cookie \
                    (object_identifier)
                if len (objects) > 1:
                    raise woodchuck.InternalError \
                        ("Internal inconsistency, this is likely a programmer "
                         + "error: multiple objects with identifier "
                         + object_identifier)
                try:
                    stream.objects[object_identifier] = objects[0]
                except IndexError:
                    raise woodchuck.NoSuchObject ("%s: object does not exist."
                                                  % (object_identifier))
        return stream.objects[object_identifier]

    def stream_updated(self, stream_identifier, indicator=0,
                       transferred_up=None, transferred_down=None,
                       download_time=None, download_duration=None,
                       new_objects=None, updated_objects=None,
                       objects_inline=None):
        stream = self._stream_lookup(stream_identifier).llstream
        stream.update_status (0, indicator, transferred_up, transferred_down,
                              download_time, download_duration,
                              new_objects, updated_objects, objects_inline)

    def stream_update_failed(self, stream_identifier, reason,
                             transferred_up=None, transferred_down=None):
        stream = self._stream_lookup(stream_identifier).llstream
        stream.update_status (reason,
                              transferred_up=transferred_up,
                              transferred_down=transferred_down)

    def stream_unregister(self, stream_identifier):
        """Unregister the indicated stream and any containing objects.
        This does not actually remove any objects' files."""
        self._stream_lookup(stream_identifier).llstream.unregister (False)
        del self.streams[stream_identifier]

    def object_register(self, stream_identifier, object_identifier,
                        human_readable_name, download_frequency,
                        expected_size=None, versions=None):
        """Register an object.  The caller can provide either
        EXPECTED_SIZE or VERSIONS, but not both."""
        assert not (expected_size is not None and versions is not None)

        properties={'cookie':object_identifier}
        if human_readable_name is not None:
            properties['human_readable_name'] = human_readable_name
        if download_frequency is not None:
            properties['download_frequency'] = download_frequency

        if versions is None and expected_size is not None:
            versions = (
                # URL, expected_size, utility, use_simple_downloader
                ("", expected_size, 0, False),
                )

        if versions is not None:
            properties['versions'] = versions

        stream = self._stream_lookup(stream_identifier)
        object = stream.llstream.object_register (True, **properties)
        stream.objects[object_identifier] = object

    def objects_list(self, stream_identifier):
        return self._stream_lookup(stream_identifier).llstream.list_objects ()

    def object_unregister(self, stream_identifier, object_identifier):
        """Unregister the indicated object.  This does not actually
        remove the object's files."""
        self._object_lookup (stream_identifier, object_identifier).unregister ()

        llstream = self._stream_lookup(stream_identifier)
        if object_identifier in llstream.objects:
            del llstream.objects[object_identifier]

    def object_downloaded(self, stream_identifier, object_identifier,
                          indicator=None,
                          transferred_up=None, transferred_down=None,
                          download_time=None, download_duration=None,
                          object_size=None, files=None):
        """Indicate that an object was successfully downloaded.  If
        the object is not yet registered, registers it with
        human_readable_name set to OBJECT_IDENTIFIER and the download
        frequnecy set to 0, i.e., a one-show download.

        INDICATOR is a bit-wise mask of woodchuck.Indicator.
        TRANSFERRED_UP and TRANSFERRED_DOWN are the number of bytes
        transferred.  Pass None if unknown.  DOWNLOAD_TIME and
        DOWNLOAD_DURATION are the time when the download started (in
        seconds since the epoch) and the time (in seconds) it took to
        perform the download.  Pass None if unknown.  OBJECT_SIZE is
        the resulting on-disk size of the object, in bytes.  Pass None
        if unknown.  FILES is an array of <FILENAME, DEDICATED,
        DELETION_POLICY>.  FILENAME is a name of a file that contains
        data from this object, DEDICATED is a boolean indicating
        whether this file is dedicated to the object (True) or shared
        with other objects (False).  DELETION_POLICY is drawn from
        woodchuck.DeletionPolicy and indicates this file's deletion
        policy."""
        stream = self._stream_lookup(stream_identifier)
        try:
            object = self._object_lookup(stream_identifier, object_identifier)
        except woodchuck.NoSuchObject:
            # Object does not exist.  Register it first.
            self.object_register (stream_identifier, object_identifier,
                                  object_identifier, 0)
            object = stream.objects[object_identifier]
            
        object.download_status (0, indicator, transferred_up, transferred_down,
                                download_time, download_duration,
                                object_size, files)

    def object_download_failed(self, stream_identifier, object_identifier,
                               reason,
                               transferred_up=None, transferred_down=None):
        """Indicate that the program failed to download an object.
        REASON is taken from woodchuck.DownloadStatus.  TRANSFERRED_UP
        and TRANSFERRED_DOWN are the number of bytes transferred.
        Pass None if unknown."""
        object = self._object_lookup(stream_identifier, object_identifier)
        object.download_status (reason, 0, transferred_up, transferred_down)

    def object_used(self, stream_identifier, object_identifier,
                    start=None, duration=None, use_mask=2**64-1):
        """Indicate that an object has been used.  START and DURATION
        are the time that the object was initially accessed (in
        seconds since the epoch) and how long the user used the object
        (in seconds).

        USE_MASK is a bit-mask indicating which parts of the object
        was used.  The least significant bit corresponds to the first
        1/64 of the object, the second-least significant bit to the
        second 1/64 of the object, etc."""
        object = self._object_lookup(stream_identifier, object_identifier)
        object.used (start, duration, use_mask)

    def object_files_deleted(self, stream_identifier, object_identifier,
                             response=woodchuck.DeletionResponse.Deleted,
                             arg=None):
        """Indicate that the files associated with an object have been
        deleted (woodchuck.DeletionResponse.Deleted), compressed
        (woodchuck.DeletionResponse.Compressed) or that a deletion
        request has been vetoed (woodchuck.DeletionResponse.Refused).
        Called when a file is deleted or in response to the
        object_files_delete upcall."""

        object = self._object_lookup(stream_identifier, object_identifier)
        object.files_deleted (response, arg)


    def object_downloaded_cb(self, stream_identifier, object_identifier,
                          status, instance, version,
                          filename, size, trigger_target, trigger_fired):
        """Virtual method that should be implemented by the child
        class if it is interested in
        org.woodchuck.upcall.ObjectDownloaded upcalls."""
        pass

    def stream_update_cb(self, stream_identifier):
        """Virtual method that should be implemented by the child
        class if it is interested in
        org.woodchuck.upcall.StreamUpdate upcalls."""
        print "PyWoodchuck.stream_update_cb called"
        pass
    
    def object_download_cb(self, stream_identifier, object_identifier,
                           version, filename, quality):
        """Virtual method that should be implemented by the child
        class if it is interested in
        org.woodchuck.upcall.StreamUpdate upcalls."""
        pass
    
    def object_delete_files_cb(self, stream_identifier, object_identifier):
        """Virtual method that should be implemented by the child
        class if it is interested in
        org.woodchuck.upcall.ObjectDeleteFiles upcalls."""
        pass
    


if __name__ == "__main__":
    import glib
    from dbus.mainloop.glib import DBusGMainLoop
    DBusGMainLoop(set_as_default=True)

    class WoodyChucky (PyWoodchuck):
        def stream_update_cb(self, stream_identifier):
            print "stream update called on %s" % (stream_identifier,)
        def object_download_cb(self, stream_identifier, object_identifier,
                               version, filename, quality):
            print "object download called on %s in stream %s" \
                % (object_identifier, stream_identifier);

    wc = WoodyChucky ("PyWoodchuck Test.", "org.woodchuck.pywoodchuck.test")
    try:
        wc.stream_register('id:a', 'A', freshness=2)
        wc.stream_register('id:b', 'B', freshness=60*60)
        try:
            failed = False
            wc.stream_register('id:b', 'B')
        except woodchuck.ObjectExistsError as exception:
            failed = True
        assert failed

        wc.stream_unregister('id:a')
        wc.stream_unregister('id:b')
        try:
            failed = False
            wc.stream_unregister('id:b')
        except woodchuck.NoSuchObject as exception:
            failed = True
        assert failed

        assert len (wc.streams_list ()) == 0

        wc.stream_register('id:a', 'A', freshness=2)
        wc.stream_register('id:b', 'B', freshness=60*60)

        assert len (wc.streams_list ()) == 2

        print "Waiting for 5 seconds for stream feedback."
        loop = glib.MainLoop ()
        glib.timeout_add_seconds (5, glib.MainLoop.quit, loop)
        loop.run ()
        print "Mainloop returned."

        wc.stream_updated('id:a', download_duration=1,
                          new_objects=3, objects_inline=3)
        wc.stream_updated('id:b', download_duration=1,
                          new_objects=3, objects_inline=3)

        for i in (1, 2, 3):
            wc.object_register('id:a', 'id:a.' + str(i), 'A.' + str(i),
                               download_frequency=2)
            print "Waiting for 5 seconds for object feedback."
            loop = glib.MainLoop ()
            glib.timeout_add_seconds (5, glib.MainLoop.quit, loop)
            loop.run ()
            print "Mainloop returned."

            if i == 3:
                wc.object_download_failed ('id:a', 'id:a.' + str(i),
                                           woodchuck.DownloadStatus.FailureGone)
            else:
                wc.object_downloaded('id:a', 'id:a.' + str(i),
                                     object_size=(100 * i))

            wc.object_downloaded('id:b', 'id:b.' + str(i),
                                 object_size=(1000 * i))

            wc.object_used ('id:b', 'id:b.' + str(i), use_mask=0x1)

        assert len (wc.objects_list ('id:a')) == 3
        assert len (wc.objects_list ('id:b')) == 3

        for i in (1, 2, 3):
            wc.object_files_deleted ('id:a', 'id:a.' + str(i))
            wc.object_unregister('id:a', 'id:a.' + str(i))
            assert len (wc.objects_list ('id:a')) == 3 - i

        wc.stream_update_failed ('id:a',
                                 woodchuck.DownloadStatus.TransientNetwork)
        wc.stream_update_failed ('id:b',
                                 woodchuck.DownloadStatus.FailureGone,
                                 890, 456)

    finally:
        try:
            wc.stream_unregister('id:a')
        except:
            pass
        try:
            wc.stream_unregister('id:b')
        except:
            pass
