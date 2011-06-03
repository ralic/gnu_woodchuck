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

#: Constant that can be passed to :func:`PyWoodchuck.stream_register`
#: indicating that the stream will never be updated.
never_updated=2 ** 32 - 1

class PyWoodchuck:
    """
    A high-level interface to Woodchuck.

    Woodchuck provides the application developer with 

    This module assumes that a single application uses the specified
    manager and that it does so in a particular way.  First, it
    assumes that the application only uses a top-level manager;
    hierarchical managers are not supported.  It also assumes that
    streams and objects are uniquely identified by their respective
    cookies (thereby allowing the use of
    :func:`_Woodchuck.lookup_manager_by_cookie`).  For most
    applications, these limitations should not present a burden.

    If applications violate these assumptions, e.g., by manipulating
    the manager in an incompatible way using a low-level interface,
    PyWoodchuck may refuse to work with the manager.

    .. note::

        In order to process upcalls, **your application must use a
        main loop**.  Moreover, DBus must know about the main loop.
        If you are using glib, **before accessing the session bus**,
        run:

          from dbus.mainloop.glib import DBusGMainLoop
          DBusGMainLoop(set_as_default=True)

        or, if you are using Qt, run:
      
          from dbus.mainloop.qt import DBusQtMainLoop
          DBusQtMainLoop(set_as_default=True)
    """

    class _upcalls(woodchuck.Upcalls):
        """Handles upcalls from the Woodchuck server and translates
        them to method calls."""
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
        """
        Registers the application with Woodchuck, if not already
        registered.

        :params human_readable_name: A string that can be shown to the
            user that identifies the application.

        :param dbus_service_name: The application's DBus service name,
            e.g., org.application.  This must be unique among all
            top-level Woodchuck managers.  (This is also used as the
            underlying manager's cookie.)  This is used by Woodchuck
            to start the application if it is not running by way of
            :func:`org.freedesktop.DBus.StartServiceByName`.

        :param request_feedback: Whether to request feedback, i.e.,
            upcalls.  If you say no here, you (currently) can't later
            enable them.  If you enable upcalls, you must use a
            mainloop.

        Example: if upcalls are not required::

            import pywoodchuck
            w = pywoodchuck.PyWoodchuck("RSS Reader", "org.rssreader")

        Example: if you are interested in the :func:`stream_update_cb`
        and :func:`object_download_cb` upcalls::

            import pywoodchuck

            class mywoodchuck (pywoodchuck.PyWoodchuck):
                def stream_update_cb(self, stream_identifier):
                    print "stream update called on %s" % (stream_identifier,)
                def object_download_cb(self,
                                       stream_identifier, object_identifier,
                                       version, filename, quality):
                    print "object download called on %s in stream %s" \\
                        % (object_identifier, stream_identifier);

            w = mywoodchuck("RSS Reader", "org.rssreader")
        """
        try:
            # Try to register a new manager.
            self.manager = woodchuck.Woodchuck().manager_register \
                (only_if_cookie_unique=True,
                 human_readable_name=human_readable_name,
                 cookie=dbus_service_name,
                 dbus_service_name=dbus_service_name)
        except woodchuck.ObjectExistsError:
            # Whoops, it failed.  Look up the manager(s) with the
            # cookie.
            self.manager = None
            for m in woodchuck.Woodchuck().lookup_manager_by_cookie \
                    (dbus_service_name, False):
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
            if ((self.__class__.object_downloaded_cb
                 == PyWoodchuck.object_downloaded_cb)
                and (self.__class__.stream_update_cb
                     == PyWoodchuck.stream_update_cb)
                and (self.__class__.object_download_cb
                     == PyWoodchuck.object_download_cb)
                and (self.__class__.object_delete_files_cb
                     == PyWoodchuck.object_delete_files_cb)):
                # None of the call backs were overriden.  There is no
                # need to subscribe.
                #
                # XXX: We should monitor __set_attr__ and if the
                # methods are eventually set, override them
                # appropriately.
                pass
            else:
                print "Instantiating upcalls"
                self.upcalls = self._upcalls (self)
                print "Instantiated upcalls"

    def stream_register(self, stream_identifier,
                        human_readable_name, freshness=0):
        """Register a new stream with Woodchuck.

        :param stream_identifier: A free-form string, which is
            uninterpreted by the server and provided on upcalls (this
            is the stream's cookie).  It must uniquely identify the
            stream within the application.  It can be an application
            specific key, e.g., the URL of an RSS feed.

        :param human_readable_name: A string that can be shown to the
            user and which should unambiguously identify the stream
            *in the context of the application*.  If the "Foo Email
            Client" manages a single inbox, setting
            human_readable_name to "Inbox" is sufficient for the user
            to identify the stream; "Foo Email Client: Inbox" is
            unnecessarily long as "Foo Email Client" is redundant.

        :param freshness: A hint to Woodchuck indicating approximately
            how often the stream should be updated, in seconds.
            (Practically, this means how often
            :func:`PyWoodchuck.stream_update_cb` will be called.)
            Woodchuck interprets 0 as meaning there are no freshness
            requirements and it is completely free to choose when to
            update the stream.  A value of :data:`never_updated` is
            interpretted as meaning that the stream is never updated
            and thus calling.

        Example::

            import pywoodchuck
            # For exception classes.
            import woodchuck

            w = pywoodchuck.PyWoodchuck("RSS Reader", "org.rssreader")

            try:
                w.stream_register('http://feeds.boingboing.net/boingboing/iBag',
                                  'BoingBoing')
            except woodchuck.ObjectExistsError as exception:
                print "Stream already registered:", exception

            w.stream_unregister ('http://feeds.boingboing.net/boingboing/iBag')
        """
        self.manager.stream_register \
            (only_if_cookie_unique=True,
             human_readable_name=human_readable_name,
             cookie=stream_identifier,
             freshness=freshness)

    def streams_list(self):
        """
        List all streams managed by this application.

        :returns: Returns an array of [`stream_identifier`,
            `human_readable_name`].

        Example::

            import pywoodchuck

            w = pywoodchuck.PyWoodchuck("Application", "org.application")
            for (stream_identifier, human_readable_name) in w.streams_list ():
                print "%s: %s" % (human_readable_name, stream_identifier)
        """
        return [(s.cookie, s.human_readable_name)
                for s in self.manager.list_streams ()]

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
        """
        Tell Woodchuck that a stream has been successfully updated.
        Call this function whenever the stream is successfully
        updated, not only in response to a :func:`stream_update_cb`
        upcall.  If a stream update fails, this should be reported
        using :func:`stream_update_failed`.

        :param stream_identifier: The stream's identifier.

        :param indicator: What indicators, if any, were shown to the
            user indicating that the stream was updated.  A bit-wise
            mask of :class:`woodchuck.Indicator`.  Default: None.

        :param transferred_up: The number of bytes uploaded.  If not
            known, set to None.  Default: None.

        :param transferred_down: The number of bytes downloaded.  If
            not known, set to None.  Default: None.

        :param download_time: The time at which the update was started
            (in seconds since the epoch).  If not known, set to None.
            Default: None.

        :param download_duration: The amount of time the update took,
            in seconds.  If not known, set to None.  Default: None.

        :param new_objects: The number of newly discovered objects.  If
            not known, set to None.  Default: None.

        :param updated_objects: The number of objects with updates.
            If not known, set to None.  Default: None.

        :param objects_inline: The number of objects whose content was
            delivered inline, i.e., with the update.  If not known,
            set to None.  Default: None.

        Example of reporting a stream update for which five new
        objects were discovered and all of which were delivered
        inline::

            import pywoodchuck
            import time

            w = pywoodchuck.PyWoodchuck("Application", "org.application")
          
            w.stream_register('stream identifier', 'human_readable_name')

            download_time = int (time.time ())
            ...
            # Perform the download
            ...
            download_duration = int (time.time ()) - download_time

            w.stream_updated ('stream identifier',
                              transferred_up=2048,
                              transferred_down=64000,
                              download_time=download_time,
                              download_duration=download_duration,
                              new_objects=5, objects_inline=5)

        .. Note:: The five new objects should immediately be
            registered using :func:`object_register` and marked as
            downloaded using :func:`object_downloaded`.
        """
        stream = self._stream_lookup(stream_identifier).llstream
        stream.update_status (0, indicator, transferred_up, transferred_down,
                              download_time, download_duration,
                              new_objects, updated_objects, objects_inline)

    def stream_update_failed(self, stream_identifier, reason,
                             transferred_up=None, transferred_down=None):
        """
        Tell Woodchuck that a stream update failed.  Call this
        function whenever a stream update is attempted, not only in
        response to a :func:`stream_update_cb` upcall.

        :param stream_identifier: The stream's identifier.

        :param reason: The reason the update failed.  Taken from
            :class:`woodchuck.DownloadStatus`.

        :param transferred_up: The number of bytes uploaded.  If not
            known, set to None.  Default: None.

        :param transferred_down: The number of bytes downloaded.  If
            not known, set to None.  Default: None.

        Example of reporting a failed stream update::

            import pywoodchuck
            import woodchuck

            w = pywoodchuck.PyWoodchuck("Application", "org.application")
          
            w.stream_register('stream identifier', 'human_readable_name')

            ...

            w.stream_update_failed ('stream identifier',
                                    woodchuck.DownloadStatus.TransientNetwork,
                                    transferred_up=1038,
                                    transferred_down=0)
        """
        stream = self._stream_lookup(stream_identifier).llstream
        stream.update_status (reason,
                              transferred_up=transferred_up,
                              transferred_down=transferred_down)

    def stream_unregister(self, stream_identifier):
        """
        Unregister the indicated stream and any containing objects.
        This just causes Woodchuck to become unaware of the stream and
        delete any metadata about it; this does not actually remove
        any objects' files.

        :param stream_identifier: The stream's identifier.

        Example: See :func:`stream_register` for an example use of
        this function.
        """
        self._stream_lookup(stream_identifier).llstream.unregister (False)
        del self.streams[stream_identifier]

    def object_register(self, stream_identifier, object_identifier,
                        human_readable_name, download_frequency=None,
                        expected_size=None, versions=None):
        """Register an object.

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.  This must
            be unique among all object's in the same stream.

        :param human_readable_name: A human readable name that can be
            shown to the user, which is unambiguous in the context of
            the stream.

        :param download_frequency: How often the object should be
            downloaded.  If 0 or None, this is a one-shot download.
            Default: None.

        :expected_size: The expected size of the download, in bytes.

        :versions: An array of [`URL`, `expected_size`, `utility`,
            use_simple_downloader`] specifying alternate versions of
            the object.  `expected_size` is the expected size of the
            download, in bytes.  `utility` is the utility of this
            version relative to other versions.  The utility is
            assumed to be a linear function, i.e.,a version with 10
            has twice as much value as another version with 5.
            `use_simple_downloader` is a boolean indicating whether
            Woodchuck should use its simple downloader to fetch the
            object.

        .. Note:: The caller may provide either `expected_size` or
            `versions`, but not both."""
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
        """
        List the objects in a stream.

        :returns: Returns an array of [`object_identifier`,
            `human_readable_name`].

        Example::

            import pywoodchuck

            w = pywoodchuck.PyWoodchuck("Application", "org.application")
            w.stream_register('stream identifier', 'human_readable_name')
            w.object_register('stream identifier',
                              'object 1', 'human_readable_name 1')
            w.object_register('stream identifier',
                              'object 2', 'human_readable_name 2')
            w.object_register('stream identifier',
                              'object 3', 'human_readable_name 3')

            for (object_identifier, human_readable_name) \\
                in w.objects_list ('stream identifier'):
                print "%s: %s" % (human_readable_name, object_identifier)

            w.object_unregister('stream identifier', 'object 2')

            for (object_identifier, human_readable_name) \\
                in w.objects_list ('stream identifier'):
                print "%s: %s" % (human_readable_name, object_identifier)

            w.stream_unregister ('stream identifier')
        """
        s = self._stream_lookup(stream_identifier).llstream
        return [(o.cookie, o.human_readable_name) for o in s.list_objects ()]

    def object_unregister(self, stream_identifier, object_identifier):
        """Unregister the indicated object.  This just causes
        Woodchuck to become unaware of the object and delete any
        associated metadata; this does not actually remove any of the
        object's files.

        See :func:`objects_list` for an example use of this function."""
        self._object_lookup (stream_identifier, object_identifier).unregister ()

        llstream = self._stream_lookup(stream_identifier)
        if object_identifier in llstream.objects:
            del llstream.objects[object_identifier]

    def object_downloaded(self, stream_identifier, object_identifier,
                          indicator=None,
                          transferred_up=None, transferred_down=None,
                          download_time=None, download_duration=None,
                          object_size=None, files=None):
        """Tell Woodchuck that an object was successfully downloaded.

        Call this function whenever a stream update is attempted, not
        only in response to a :func:`stream_update_cb` upcall.

        If the object is not yet registered, registers it with its
        `human_readable_name` set to `object_identifier` and the
        download frequnecy set to 0, i.e., as a one-shot download.

        :param indicator: What indicators, if any, were shown to the
            user indicating that the stream was updated.  A bit-wise
            mask of :class:`woodchuck.Indicator`.  Default: None.

        :param transferred_up: The number of bytes uploaded.  If not
            known, set to None.  Default: None.

        :param transferred_down: The number of bytes downloaded.  If
            not known, set to None.  Default: None.

        :param download_time: The time at which the update was started
            (in seconds since the epoch).  If not known, set to None.
            Default: None.

        :param download_duration: The amount of time the update took,
            in seconds.  If not known, set to None.  Default: None.

        :param object_size: The resulting on-disk size of the object,
            in bytes.  Pass None if unknown.  Default: None.

        :param files: An array of [`filename`, `dedicated`,
            `deletion_policy`] arrays.  `filename` is the name of a
            file that contains data from this object; `dedicated` is a
            boolean indicating whether this file is dedicated to the
            object (True) or shared with other objects (False);
            `deletion_policy` is drawn from woodchuck.DeletionPolicy
            and indicates this file's deletion policy.

        Example::

            import pywoodchuck
            import woodchuck

            w = pywoodchuck.PyWoodchuck("Podcasts", "org.podcasts")
            w.stream_register("http://podcast.site/podcasts/SomePodcast.rss",
                              "Some Podcast")
            w.object_register(
                "http://podcast.site/podcasts/SomePodcast.rss",
                "http://podcast.site/podcasts/SomePodcast/Episode-15.ogg",
                "Episode 15: Title")

            ...

            w.object_downloaded (
                "http://podcast.site/podcasts/SomePodcast.rss",
                "http://podcast.site/podcasts/SomePodcast/Episode-15.ogg",
                indicator=(woodchuck.Indicator.ApplicationVisual
                           |woodchuck.Indicator.DesktopSmallVisual
                           |woodchuck.Indicator.ObjectSpecific),
                transferred_up=39308, transferred_down=991203,
                files=[ ["/home/user/Podcasts/SomePodcast/Episode-15.ogg",
                         True,
                         woodchuck.DeletionPolicy.DeleteWithoutConsultation], ])

            w.stream_unregister ("http://podcast.site/podcasts/SomePodcast.rss")
        """
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

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.

        :param reason: The reason the update failed.  Taken from
            :class:`woodchuck.DownloadStatus`.

        :param transferred_up: The number of bytes uploaded.  If not
            known, set to None.  Default: None.

        :param transferred_down: The number of bytes downloaded.  If
            not known, set to None.  Default: None.

        Example: For an example of a similar function, see
        :func:`stream_update_failed`.
        """
        object = self._object_lookup(stream_identifier, object_identifier)
        object.download_status (reason, 0, transferred_up, transferred_down)

    def object_used(self, stream_identifier, object_identifier,
                    start=None, duration=None, use_mask=0xffffffffffffffff):
        """
        Indicate that an object has been used.

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.

        :param start: The time that the use of the object started, in
            seconds since the epoch.

        :param duration: The amount of time that the object was used,
            in seconds.

        :param use_mask: A 64-bit bit-mask indicating which parts of
            the object was used.  Setting the least significant bit
            means the first 1/64 of the object was used, the
            second-least significant bit that the second 1/64 of the
            object, etc.

        Example: indicate that the user view the first 2 minutes of a
        64 minute video Podcast::

            import pywoodchuck
            import time

            w = pywoodchuck.PyWoodchuck("Podcasts", "org.podcasts")
            w.stream_register("http://videocast.site/podcasts/Videocast.rss",
                              "Video Podcast")
            w.object_register(
                "http://videocast.site/podcasts/Videocast.rss",
                "http://videocast.site/podcasts/Episode-15.ogv",
                "Episode 15: Title")

            ...

            # User clicks play:
            start = int (time.time ())
            use_mask = 0
            length = 64

            ...

            # Periodically sample the stream's position and update use_mask.
            for pos in (1, 2):
                use_mask |= 1 << int (64 * (pos / float (length)) - 1)

            ...

            # User clicks stop after 2 minutes.  `use_mask` is now
            # 0x3: the least two significant bits are set.
            end = int (time.time ())

            w.object_used (
                "http://videocast.site/podcasts/Videocast.rss",
                "http://videocast.site/podcasts/Episode-15.ogv",
                start, end - start, use_mask)

            w.stream_unregister ("http://videocast.site/podcasts/Videocast.rss")
        """
        object = self._object_lookup(stream_identifier, object_identifier)
        object.used (start, duration, use_mask)

    def object_files_deleted(self, stream_identifier, object_identifier,
                             update=woodchuck.DeletionResponse.Deleted,
                             arg=None):
        """
        Indicate that the files associated with an object have been
        deleted, compressed (e.g., an email attachment, but not the
        body, was deleted) or that a deletion request has been vetoed,
        because, e.g., the application thinks the user still needs the
        data.

        .. Note: Call function not only in response to the
            :func:`object_files_delete_cb` upcall, but whenever files
            with a registered object are deleted.

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.

        :param update: The type of update.  Taken from
            :class:`woodchuck.DeletionResponse`.

        :param arg: If update is woodchuck.DeletionResponse.Deleted,
            the value is ignored.

            If update is woodchuck.DeletionResponse.Refused, the value
            is the minimum number of seconds the object should be
            preserved, i.e., the minimum amount of time before
            Woodchuck should call
            :func:`Upcalls.object_delete_files_cb` again.

            If update is woodchuck.DeletionResponse.Compressed, the
            value is the number of bytes of disk space the object now
            uses.

        Example: Indicating that an email attachment has been deleted,
        but not the email's body::

            import pywoodchuck
            import woodchuck

            w = pywoodchuck.PyWoodchuck("HMail", "org.hmail")
            w.stream_register("user@provider.com/INBOX", "Provider Inbox")

            w.object_register(
                "user@provider.com/INBOX",
                "2721812449",
                "Subject Line")

            ...

            w.object_downloaded (
                "user@provider.com/INBOX", "2721812449",
                transferred_up=3308, transferred_down=991203,
                files=[ ["/home/user/Maildir/.inbox/cur/2721812449",
                         True,
                         woodchuck.DeletionPolicy.DeleteWithConsultation], ])

            w.object_files_deleted ("user@provider.com/INBOX", "2721812449",
                                    woodchuck.DeletionResponse.Compressed, 1877)
            
            w.stream_unregister ("user@provider.com/INBOX")
        """
        object = self._object_lookup(stream_identifier, object_identifier)
        object.files_deleted (update, arg)


    def object_downloaded_cb(self, stream_identifier, object_identifier,
                             status, instance, version,
                             filename, size, trigger_target, trigger_fired):
        """Virtual method that should be implemented by the child
        class if it is interested in receiving object downloaded
        notifications (:func:`org.woodchuck.upcall.ObjectDownloaded`).

        This upcall is invoked when Woodchuck downloads an object on
        behalf of a manager.  This is only done for objects using the
        simple downloader.

        .. note: Woodchuck only downloads objects if it is explicitly
            told to by way of the `use_simple_downloader` property.
            See the `versions` parameter to :func:`object_register`
            for more details.

            If you don't use this option, there is no need to
            implement this method.

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.

        :param status: Whether the download was successfully.  The
            value is taken from :class:`woodchuck.DownloadStatus`.

        :param instance: The number of download attempts (not
            including this one).

        :param version: The version that was downloaded.  An array of:
            the index in the version array, the URL, the expected
            size, the utility and the value of use simple downloader.

        :param filename: The name of the file containing the data.

        :param size: The size of the file, in bytes.

        :param trigger_target: The time the application requested the
            object be downloaded.

        :param trigger_fired: The time at which the file was actually
            downloaded.

        Example: for an example of how to implement an upcall, see the
        opening example to :class:`PyWoodchuck`.
        """
        pass

    def stream_update_cb(self, stream_identifier):
        """
        Virtual method that should be implemented by the child
        class if it is interested in receiving stream update
        notifications (:func:`org.woodchuck.upcall.StreamUpdate`).

        This upcall is invoked when a stream should be updated.  The
        application should update the stream and call
        :func:`stream_updated` or :func:`stream_update_failed`, as
        appropriate.

        .. note: If all streams are marked as never receiving updates
            (see the `freshness` parameter to
            :func:`stream_register`), there is no need to implement
            this upcall.

        :param stream_identifier: The stream's identifier.

        Example: for an example of how to implement an upcall, see the
        opening example to :class:`PyWoodchuck`.
        """
        pass
    
    def object_download_cb(self, stream_identifier, object_identifier,
                           version, filename, quality):
        """
        Virtual method that should be implemented by the child class
        if it is interested in receiving object download notifications
        (:func:`org.woodchuck.upcall.ObjectDownload`).

        This upcall is invoked when an object should be downloaded.
        The application should download the object and call either
        :func:`object_downloaded` or :func:`object_download_failed`,
        as appropriate.

        .. note: If all objects are marked as using the simple
            downloader (see the `versions` parameter to
            :func:`object_register` for more details), there is no
            need to implement this upcall.

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.

        :param version: The version to download.  An array of: the
            index in the version array, the URL, the expected size,
            the utility and the value of use simple downloader.

        :param filename: The name of the filename property.

        :param quality: The degree to which quality should be
            sacrified to reduce the number of bytes transferred.  The
            target quality of the download.  From 1 (most compressed)
            to 5 (highest available fidelity).

        Example: for an example of how to implement an upcall, see the
        opening example to :class:`PyWoodchuck`.
        """
        pass
    
    def object_delete_files_cb(self, stream_identifier, object_identifier):
        """Virtual method that should be implemented by the child
        class if it is interested in receiving deletion requests
        (:func:`org.woodchuck.upcall.ObjectDeleteFiles`).

        This upcall is invoked when an object's files should be
        downloaded.  The application should respond with
        :func:`object_files_deleted`.

        .. note: If no objects are marked having the deletion policy
            :data:woodchuck.DeletionPolicy.DeleteWithConsultation`,
            there is no need to implement this upcall.

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.

        Example: for an example of how to implement an upcall, see the
        opening example to :class:`PyWoodchuck`.
        """
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
