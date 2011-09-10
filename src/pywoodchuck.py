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
from UserDict import DictMixin
from weakref import WeakValueDictionary

def _singleton(cls, identifier, *init_args):
    """
    Ensure that there is only a single instance of a given object type
    with the specified identifier.
    """
    if not hasattr (cls, '_singletons'):
        # Don't create a strong reference.
        cls._singletons = WeakValueDictionary()
    o = cls._singletons.get (identifier, None)
    if o is None:
        # print "%s: Creating %s" % (cls.__name__, id)
        o = cls(*init_args)
        cls._singletons[identifier] = o
    else:
        assert llobject == o.llobject

    return o

def Object(stream, llobject):
    """
    Object factory.
    """
    return _singleton (_Object, llobject.cookie, stream, llobject)

def Stream(pywoodchuck, llobject):
    """
    Stream factory.
    """
    return _singleton (_Object, llobject.cookie, pywoodchuck, llobject)

class _BaseObject(object):
    """
    Code common to the _Object and _Stream classes.
    """
    def __init__(self, containing_dict, llobject):
        super (_BaseObject, self).__init__ ()

        self.containing_dict = containing_dict
        self.llobject = llobject

    def __repr__(self):
        return ("pywoodchuck." + self.__class__.__name__ + "("
                + {'identifier': self.cookie,
                   'human_readable_name': self.human_readable_name}.__repr__ ()
                + ")")

    def __getattribute__(self, name):
        """
        If name matches a property name, return the value of the
        property.  Otherwise, return the value of the instance's
        attribute corresponding to name.
        """
        if name == 'identifier':
            name = 'cookie'

        if name != 'property_list' and name in self.property_list:
            return self.llobject.__getattribute__ (name)
        return super (_BaseObject, self).__getattribute__ (name)

    def __setattr__(self, name, value):
        """
        If name matches a property name, set the value of the
        property.  Otherwise, return the value of the instance's
        attribute corresponding to name.
        """
        if name == 'identifier':
            name = 'cookie'

        if name in self.property_list:
            if name == 'cookie':
                # Rename the entry in the containing map.
                oldvalue = self['cookie']
            self.llobject.__setattr__ (name, value)
            if name == 'cookie':
                assert oldvalue in self.containing_dict
                del self.containing_dict[oldvalue]
                self.containing_dict[value] = self
        else:
            return super (_BaseObject, self).__setattr__ (name, value)

class _Object(_BaseObject):
    """
    Encapsulates a Woodchuck object.  This object should never be
    explicitly instantiated by user code.  Instead, use
    PyWoodchuck[stream_identifier][object_identifier] to obtain a
    reference to an instance.

    Object properties, such as publication time, can be gotten and set
    by assigning to the like named instance attributes, e.g.::

        object.publication_time = time.time ()
    """
    property_list = woodchuck.object_properties

    def __init__(self, stream, llobject):
        """
        :param stream: The stream containing the object (an instance
            of :class:`_Stream`).

        :param llobject: The low-level object representing the object
            (an instance of :class:`woodchuck._Object`).
        """
        assert isinstance (stream, _Stream)
        assert isinstance (llobject, woodchuck._Object)

        super (_Object, self).__init__ (stream._objects, llobject)

        self.stream = stream

    def unregister(self):
        """
        Unregister the object.  This just causes Woodchuck to become
        unaware of the object and delete any associated metadata; this
        does not actually remove any of the object's files.

        .. Note::

            This function is an alias for::

                del pywoodchuck[stream_identifier][object_identifier]
        """
        object_identifier = self.cookie
        assert object_identifier in self.containing_dict
        assert self.containing_dict[object_identifier] == self

        self.llobject.unregister ()
        del self.containing_dict[object_identifier]

    def transferred(self, indicator=None,
                    transferred_up=None, transferred_down=None,
                    transfer_time=None, transfer_duration=None,
                    object_size=None, files=None):
        """
        Tell Woodchuck that the object was successfully transferred.

        Call this function whenever an object transfer is attempted,
        not only in response to a :func:`object_transfer_cb` upcall.

        :param indicator: What indicators, if any, were shown to the
            user indicating that the stream was updated.  A bit-wise
            mask of :class:`woodchuck.Indicator`.  Default: None.

        :param transferred_up: The number of bytes uploaded.  If not
            known, set to None.  Default: None.

        :param transferred_down: The number of bytes transferred.  If
            not known, set to None.  Default: None.

        :param transfer_time: The time at which the update was started
            (in seconds since the epoch).  If not known, set to None.
            Default: None.

        :param transfer_duration: The amount of time the update took,
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
            w.stream_register("http://podcast.site/SomePodcast.rss",
                              "Some Podcast")
            w["http://podcast.site/SomePodcast.rss"].object_register(
                "http://podcast.site/SomePodcast/Episode-15.ogg",
                "Episode 15: Title")

            # Transfer the file.

            w["http://podcast.site/SomePodcast.rss"]\\
                ["http://podcast.site/SomePodcast/Episode-15.ogg"].transferred(
                indicator=(woodchuck.Indicator.ApplicationVisual
                           |woodchuck.Indicator.DesktopSmallVisual
                           |woodchuck.Indicator.ObjectSpecific),
                transferred_up=39308, transferred_down=991203,
                files=[ ["/home/user/SomePodcast/Episode-15.ogg",
                         True,
                         woodchuck.DeletionPolicy.DeleteWithoutConsultation], ])

            del w["http://podcast.site/SomePodcast.rss"]
        """
        self.llobject.transfer_status (
            0, indicator, transferred_up, transferred_down,
            transfer_time, transfer_duration,
            object_size, files)

    def transfer_failed(self, reason,
                        transferred_up=None, transferred_down=None):
        """
        Indicate that the program failed to transfer the object.

        :param reason: The reason the update failed.  Taken from
            :class:`woodchuck.TransferStatus`.

        :param transferred_up: The number of bytes uploaded.  If not
            known, set to None.  Default: None.

        :param transferred_down: The number of bytes transferred.  If
            not known, set to None.  Default: None.

        Example: For an example of a similar function, see
        :func:`_Stream.stream_update_failed`.
        """
        self.llobject.transfer_status (
            reason, 0, transferred_up, transferred_down)

    def used(self, start=None, duration=None, use_mask=0xffffffffffffffff):
        """
        Indicate that the object has been used.

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
            w["http://videocast.site/podcasts/Videocast.rss"].object_register(
                "http://videocast.site/podcasts/Episode-15.ogv",
                "Episode 15: Title")

            # User clicks play:
            start = int (time.time ())
            use_mask = 0
            length = 64

            # Periodically sample the stream's position and update use_mask.
            for pos in (1, 2):
                use_mask |= 1 << int (64 * (pos / float (length)) - 1)

            # User clicks stop after 2 minutes.  `use_mask` is now
            # 0x3: the least two significant bits are set.
            end = int (time.time ())

            w["http://videocast.site/podcasts/Videocast.rss"]\\
                ["http://videocast.site/podcasts/Episode-15.ogv"].used (
                start, end - start, use_mask)

            del w["http://videocast.site/podcasts/Videocast.rss"]
        """
        self.llobject.used (start, duration, use_mask)

    def files_deleted(self, update=woodchuck.DeletionResponse.Deleted,
                      arg=None):
        """
        Indicate that the files associated with the object have been
        deleted, compressed (e.g., an email attachment, but not the
        body, was deleted) or that a deletion request has been vetoed,
        because, e.g., the application thinks the user still needs the
        data.

        .. Note: Call function not only in response to the
            :func:`object_files_delete_cb` upcall, but whenever files
            with a registered object are deleted.

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

            w["user@provider.com/INBOX"].object_register(
                "2721812449",
                "Subject Line")

            w["user@provider.com/INBOX"]["2721812449"].transferred (
                transferred_up=3308, transferred_down=991203,
                files=[ ["/home/user/Maildir/.inbox/cur/2721812449",
                         True,
                         woodchuck.DeletionPolicy.DeleteWithConsultation], ])

            w["user@provider.com/INBOX"]["2721812449"].files_deleted (
                woodchuck.DeletionResponse.Compressed, 1877)
            
            del w["user@provider.com/INBOX"]
        """
        self.llobject.files_deleted (update, arg)

class _Stream(_BaseObject, DictMixin):
    """
    Encapsulates a Woodchuck stream.  This object should never be
    explicitly instantiated by user code.  Instead, use
    PyWoodchuck[stream_identifier] to obtain a reference to an
    instance.

    A :class:`_Stream` instance behaves like a dictionary: iterating
    over it yields the objects contained therein; objects can be
    indexed by their object identifier; and, objects can also be
    removed (:func:`_Object.unregister`) using del.  Note: you
    *cannot* register an object by assigning a value to a key.

    Stream properties, such as freshness, can be get and set by
    assigning to the like named instance attributes, e.g.::

        stream.freshness = 60 * 60
    """
    property_list = woodchuck.stream_properties

    def __init__(self, pywoodchuck, llobject):
        """
        :param pywoodchuck: The pywoodchuck instance containing the
            stream (an instance of :class:`PyWoodchuck`).

        :param llobject: The low-level object representing the stream
            (an instance of :class:`woodchuck._Stream`).
        """
        assert isinstance (pywoodchuck, PyWoodchuck)
        assert isinstance (llobject, woodchuck._Stream)

        super (_Stream, self).__init__ (pywoodchuck._streams, llobject)

        self.pywoodchuck = pywoodchuck

        # List of locally known objects, i.e., a subset of the actual
        # objects.
        self._objects = {}

    def _object_lookup(self, object_identifier, object_UUID=None):
        """
        If object_UUID is supplied, it is assumed that an object with
        the supplied object identifier and object_UUID exists.
        """
        if object_identifier not in self._objects:
            if object_UUID is not None:
                self._objects[object_identifier] \
                    = _Object (self,
                               woodchuck.Object (UUID=object_UUID,
                                                 cookie=object_identifier))
            else:
                objects = self.llobject.lookup_object_by_cookie (
                    object_identifier)
                if len (objects) > 1:
                    raise woodchuck.InternalError (
                        "Internal inconsistency, this is likely a programmer "
                        + "error: multiple objects with identifier "
                        + object_identifier)
                if len (objects) == 0:
                    raise woodchuck.NoSuchObject ("%s: object does not exist."
                                                  % (object_identifier,))

                self._objects[object_identifier] = _Object (self, objects[0])
        return self._objects[object_identifier]

    def __getitem__(self, object_identifier):
        try:
            return self._object_lookup (object_identifier)
        except woodchuck.NoSuchObject, exception:
            raise KeyError (exception.args[0])

    def __contains__(self, object_identifier):
        try:
            self.__getitem__ (object_identifier)
            return True
        except KeyError:
            return False

    def __delitem__(self, object_identifier):
        """
        .. Note::

            Equivalent to calling::

                object[object_identifier].:func:`_Object.unregister`.
        """
        self[object_identifier].unregister ()

    def keys(self):
        return [o.cookie for o in self.llobject.list_objects ()]

    def __len__(self):
        return len (self.llobject.list_objects ())

    def unregister(self):
        """
        Unregister the stream and any objects it contains.  This just
        causes Woodchuck to become unaware of the stream and delete
        any metadata about it; this does not actually remove any
        objects' files.

        .. Note::

            This function is eqivalent to calling::

                del pywoodchuck[stream_identifier]

            

        Example: See :func:`PyWoodchuck.stream_register` for an
        example use of this function.
        """
        stream_identifier = self.cookie
        assert stream_identifier in self.containing_dict
        assert self.containing_dict[stream_identifier] is self

        self.llobject.unregister (only_if_empty=False)
        del self.containing_dict[stream_identifier]

    def updated(self, indicator=0,
                transferred_up=None, transferred_down=None,
                transfer_time=None, transfer_duration=None,
                new_objects=None, updated_objects=None,
                objects_inline=None):
        """
        Tell Woodchuck that the stream has been successfully updated.
        Call this function whenever the stream is successfully
        updated, not only in response to a :func:`stream_update_cb`
        upcall.  If a stream update fails, this should be reported
        using :func:`_Stream.update_failed`.

        :param indicator: What indicators, if any, were shown to the
            user indicating that the stream was updated.  A bit-wise
            mask of :class:`woodchuck.Indicator`.  Default: None.

        :param transferred_up: The number of bytes uploaded.  If not
            known, set to None.  Default: None.

        :param transferred_down: The number of bytes transferred.  If
            not known, set to None.  Default: None.

        :param transfer_time: The time at which the update was started
            (in seconds since the epoch).  If not known, set to None.
            Default: None.

        :param transfer_duration: The amount of time the update took,
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
          
            w.stream_register("stream identifier", "human_readable_name")

            transfer_time = int (time.time ())

            # Perform the transfer

            transfer_duration = int (time.time ()) - transfer_time

            w["stream identifier"].updated (
                transferred_up=2048, transferred_down=64000,
                transfer_time=transfer_time,
                transfer_duration=transfer_duration,
                new_objects=5, objects_inline=5)

            del w["stream identifier"]

        .. Note:: The five new objects should immediately be
            registered using :func:`object_register` and marked as
            transferred using :func:`_Object.transferred`.
        """
        self.llobject.update_status (
            0, indicator, transferred_up, transferred_down,
            transfer_time, transfer_duration,
            new_objects, updated_objects, objects_inline)

    def update_failed(self, reason, transferred_up=None, transferred_down=None):
        """
        Tell Woodchuck that a stream update failed.  Call this
        function whenever a stream update is attempted, not only in
        response to a :func:`stream_update_cb` upcall.

        :param reason: The reason the update failed.  Taken from
            :class:`woodchuck.TransferStatus`.

        :param transferred_up: The number of bytes uploaded.  If not
            known, set to None.  Default: None.

        :param transferred_down: The number of bytes transferred.  If
            not known, set to None.  Default: None.

        Example of reporting a failed stream update::

            import pywoodchuck
            import woodchuck

            w = pywoodchuck.PyWoodchuck("Application", "org.application")
          
            w.stream_register("stream identifier", "human_readable_name")

            # Try to transfer the data.

            w["stream identifier"].update_failed (
                woodchuck.TransferStatus.TransientNetwork,
                transferred_up=1038, transferred_down=0)

            del w["stream identifier"]
        """
        self.llobject.update_status (reason,
                                     transferred_up=transferred_up,
                                     transferred_down=transferred_down)

    def object_register(self, object_identifier,
                        human_readable_name, transfer_frequency=None,
                        expected_size=None, versions=None):
        """Register an object.

        :param object_identifier: The object's identifier.  This must
            be unique among all object's in the same stream.

        :param human_readable_name: A human readable name that can be
            shown to the user, which is unambiguous in the context of
            the stream.

        :param transfer_frequency: How often the object should be
            transferred.  If 0 or None, this is a one-shot transfer.
            Default: None.

        :expected_size: The expected amount of disk space required
            after this transfer completes.  If this object represents
            an upload and space will be freed after the transfer
            completes, this should be negative.

        :versions: An array of [`URL`, `expected_size`,
            `expected_transfer_up`, `expected_transfer_down`,
            `utility`, use_simple_transferer`] specifying alternate
            versions of the object.  `expected_size` is the expected
            amount of disk space required when this transfer
            completes.  `expected_transfer_up` is the expected upload
            size, in bytes.  `expected_transfer_down` is the expected
            transfer size, in bytes.  `utility` is the utility of this
            version relative to other versions.  The utility is
            assumed to be a linear function, i.e.,a version with 10
            has twice as much value as another version with 5.
            `use_simple_transferer` is a boolean indicating whether
            Woodchuck should use its simple transferer to fetch the
            object.

        :returns: Returns a :class:`_Object` instance.

        .. Note:: The caller may provide either `expected_size` or
            `versions`, but not both."""
        assert not (expected_size is not None and versions is not None)

        properties={'cookie':object_identifier}
        if human_readable_name is not None:
            properties['human_readable_name'] = human_readable_name
        if transfer_frequency is not None:
            properties['transfer_frequency'] = transfer_frequency

        if versions is None and expected_size is not None:
            versions = (
                # URL, expected_size, expected_transfer_up,
                # expected_transfer_down, utility, use_simple_transferer
                ("", expected_size, 0, 0, 0, False),
                )

        if versions is not None:
            properties['versions'] = versions

        llobject = self.llobject.object_register (True, **properties)
        self._objects[object_identifier] = _Object (self, llobject)

        return self._objects[object_identifier]

    def objects_list(self):
        """
        List the objects in the stream.

        :returns: Returns a list of :class:`_Object` instances.

        .. Note:: This function is equivalent to iterating over the
            stream::

                for obj in stream.values ():
                    print obj.identifier, obj.human_readable_name

        Example::

            import pywoodchuck

            w = pywoodchuck.PyWoodchuck("Application", "org.application")
            w.stream_register("stream identifier", "human_readable_name")
            w["stream identifier"].object_register(
                "object 1", "human_readable_name 1")
            w["stream identifier"].object_register(
                "object 2", "human_readable_name 2")
            w["stream identifier"].object_register(
                "object 3", "human_readable_name 3")

            for obj in w["stream identifier"].objects_list ():
                print "%s: %s" % (obj.human_readable_name, obj.identifier)

            del w["stream identifier"]["object 2"]

            for obj in w["stream identifier"].objects_list ():
                print "%s: %s" % (obj.human_readable_name, obj.identifier)

            del w["stream identifier"]
        """
        return [Object (self, o) for o in self.llobject.list_objects()]

    def object_transferred(self, object_identifier, *args, **kwargs):
        """Tell Woodchuck that an object was successfully transferred.

        This function is a wrapper for :func:`_Object.transferred`.  It
        takes one additional argument, the object's identifier.  Like
        :func:`_Object.transferred`, this function marks the object as
        transferred.  Unlike :func:`_Object.transferred`, if the object
        is not yet registered, this function first registers it
        setting `human_readable_name` set to `object_identifier`.

        Example::

            import pywoodchuck
            import woodchuck

            w = pywoodchuck.PyWoodchuck("Podcasts", "org.podcasts")
            w.stream_register("http://podcast.site/podcasts/SomePodcast.rss",
                              "Some Podcast")
            w["http://podcast.site/podcasts/SomePodcast.rss"].object_transferred(
                "http://podcast.site/podcasts/SomePodcast/Episode-15.ogg",
                indicator=(woodchuck.Indicator.ApplicationVisual
                           |woodchuck.Indicator.DesktopSmallVisual
                           |woodchuck.Indicator.ObjectSpecific),
                transferred_up=39308, transferred_down=991203,
                files=[ ["/home/user/Podcasts/SomePodcast/Episode-15.ogg",
                         True,
                         woodchuck.DeletionPolicy.DeleteWithoutConsultation], ])

            del w["http://podcast.site/podcasts/SomePodcast.rss"]
        """
        try:
            object = self[object_identifier]
        except KeyError:
            # Object does not exist.  Register it first.
            self.object_register (object_identifier, object_identifier, 0)
            object = self._objects[object_identifier]

        object.transferred (*args, **kwargs)

    def object_transfer_failed(self, object_identifier, *args, **kwargs):
        """
        Indicate that the program failed to transfer the object.

        This function is a wrapper for
        :func:`_Object.transfer_failed`.  It takes one additional
        argument, the object's identifier.  Like
        :func:`_Object.transfer_failed`, this function marks the
        object as having failed to be transferred.  Unlike
        :func:`_Object.transfer_failed`, if the object is not yet
        registered, this function first registers it setting
        `human_readable_name` set to `object_identifier`.
        """
        try:
            object = self[object_identifier]
        except KeyError:
            # Object does not exist.  Register it first.
            self.object_register (object_identifier, object_identifier, 0)
            object = self._objects[object_identifier]

        return self[object_identifier].transfer_failed (*args, **kwargs)

    def object_files_deleted(self, object_identifier, *args, **kwargs):
        """
        Indicate that the files associated with the object have been
        deleted, compressed (e.g., an email attachment, but not the
        body, was deleted) or that a deletion request has been vetoed,
        because, e.g., the application thinks the user still needs the
        data.

        .. Note::

            This function is an alias for :func:`_Object.files_deleted`::

                pywoodchuck[stream_identifier][object_identifier].files_deleted (...)

        :param object_identifier: The object's identifier.

        The remaining parameters are passed through to
        :func:`_Object.files_deleted`.
        """
        return self[object_identifier].files_deleted (*args, **kwargs)

class PyWoodchuck(DictMixin):
    """
    A high-level, pythonic interface to Woodchuck.

    This module assumes that a single application uses the specified
    manager and that it does so in a particular way.  First, it
    assumes that the application only uses a top-level manager;
    hierarchical managers are not supported.  It also assumes that
    streams and objects are uniquely identified by their respective
    cookies (thereby allowing the use of
    :func:`org.woodchuck.LookupManagerByCookie`).  For most
    applications, these limitations should not present a burden.

    If applications violate these assumptions, i.e., by manipulating
    the manager in an incompatible way using a low-level interface,
    PyWoodchuck may refuse to work with the manager.

    .. note::

        In order to process upcalls, **your application must use a
        main loop**.  Moreover, DBus must know about the main loop.
        If you are using glib, **before accessing the session bus**,
        run::

          from dbus.mainloop.glib import DBusGMainLoop
          DBusGMainLoop(set_as_default=True)

        or, if you are using Qt, run::
      
          from dbus.mainloop.qt import DBusQtMainLoop
          DBusQtMainLoop(set_as_default=True)

    A :class:`PyWoodchuck` instance behaves like a dictionary:
    iterating over it yields the streams contained therein; streams
    can be indexed by their stream identifier; and, stream can also be
    removed (:func:`_Stream.unregister`) using del.  Note: you
    *cannot* register a stream by assigning a value to a key.
    """

    class _upcalls(woodchuck.Upcalls):
        """Handles upcalls from the Woodchuck server and translates
        them to method calls."""
        def __init__(self, pywoodchuck):
            self.pywoodchuck = pywoodchuck
            try:
                woodchuck.Upcalls.__init__(self, "/org/woodchuck")
            except woodchuck.WoodchuckUnavailableError:
                return
            self.pywoodchuck.manager.feedback_subscribe (False)

        def object_transferred_cb (self, manager_UUID, manager_cookie,
                                   stream_UUID, stream_cookie,
                                   object_UUID, object_cookie,
                                   status, instance, version, filename, size,
                                   trigger_target, trigger_fired):
            """org.woodchuck.upcalls.ObjectTransferred"""
            try:
                stream = self.pywoodchuck._stream_lookup (
                    stream_cookie, stream_UUID)
                object = stream._object_lookup (object_cookie, object_UUID)
            except woodchuck.NoSuchObject, exception:
                print "Woodchuck invoked " \
                    "org.woodchuck.upcall.ObjectTransferred", \
                    "for non-existant object: ", str (exception)
                return False

            self.pywoodchuck.object_transferred_cb (
                stream, object, status, instance, version, filename, size,
                trigger_target, trigger_fired)

        def stream_update_cb(self, manager_UUID, manager_cookie,
                             stream_UUID, stream_cookie):
            """org.woodchuck.upcalls.StreamUpdate."""
            try:
                stream = self.pywoodchuck._stream_lookup \
                    (stream_cookie, stream_UUID)
            except woodchuck.NoSuchObject, exception:
                print "Woodchuck invoked " \
                    "org.woodchuck.upcall.ObjectTransferred", \
                    "for non-existant object: ", str (exception)
                return False

            self.pywoodchuck.stream_update_cb (stream)

        def object_transfer_cb (self, manager_UUID, manager_cookie,
                                stream_UUID, stream_cookie,
                                object_UUID, object_cookie,
                                version, filename, quality):
            """org.woodchuck.upcalls.ObjectTransfer"""
            try:
                stream = self.pywoodchuck._stream_lookup (
                    stream_cookie, stream_UUID)
                object = stream._object_lookup (object_cookie, object_UUID)
            except woodchuck.NoSuchObject, exception:
                print "Woodchuck invoked " \
                    "org.woodchuck.upcall.ObjectTransfer", \
                    "for non-existant object: ", str (exception)
                return False

            self.pywoodchuck.object_transfer_cb (stream, object,
                                                 version, filename, quality)

        def object_delete_files_cb (self, manager_UUID, manager_cookie,
                                    stream_UUID, stream_cookie,
                                    object_UUID, object_cookie):
            """org.woodchuck.upcalls.ObjectTransfer"""
            try:
                stream = self.pywoodchuck._stream_lookup (
                    stream_cookie, stream_UUID)
                object = stream._object_lookup (object_cookie, object_UUID)
            except woodchuck.NoSuchObject, exception:
                print "Woodchuck invoked " \
                    "org.woodchuck.upcall.ObjectDeleteFiles", \
                    "for non-existant object: ", str (exception)
                return False

            self.pywoodchuck.object_delete_files_cb (stream, object)

    def __init__(self, human_readable_name, dbus_service_name,
                 request_feedback=True):
        """
        Registers the application with Woodchuck, if not already
        registered.

        :param human_readable_name: A string that can be shown to the
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
        and :func:`object_transfer_cb` upcalls::

            import pywoodchuck

            class mywoodchuck (pywoodchuck.PyWoodchuck):
                def stream_update_cb(self, stream):
                    print "stream update called on %s" % (stream.identifier,)
                def object_transfer_cb(self, stream, object,
                                       version, filename, quality):
                    print "object transfer called on %s in stream %s" \\
                        % (object.identifier, stream.identifier);

            w = mywoodchuck("RSS Reader", "org.rssreader")

        The returned object behaves like a dict, which maps stream
        identifiers to :class:`_Stream` objects.
        """
        try:
            # Try to register a new manager.
            self.manager = woodchuck.Woodchuck().manager_register \
                (only_if_cookie_unique=True,
                 human_readable_name=human_readable_name,
                 cookie=dbus_service_name,
                 dbus_service_name=dbus_service_name)
        except woodchuck.WoodchuckUnavailableError, exception:
            print "Unable to connect to Woodchuck server:", str (exception)
            self.manager = None
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
        self._streams = {}

        if request_feedback:
            if ((self.__class__.object_transferred_cb
                 == PyWoodchuck.object_transferred_cb)
                and (self.__class__.stream_update_cb
                     == PyWoodchuck.stream_update_cb)
                and (self.__class__.object_transfer_cb
                     == PyWoodchuck.object_transfer_cb)
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
                self.upcalls = self._upcalls (self)

    def _stream_lookup(self, stream_identifier, UUID=None):
        if stream_identifier not in self._streams:
            if UUID is not None:
                self._streams[stream_identifier] \
                    = _Stream(self,
                              woodchuck.Stream (UUID=UUID,
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
                self._streams[stream_identifier] = _Stream(self, streams[0])
        return self._streams[stream_identifier]

    def __getitem__(self, stream_identifier):
        try:
            return self._stream_lookup (stream_identifier)
        except woodchuck.NoSuchObject, exception:
            raise KeyError (exception.args[0])

    def __contains__(self, stream_identifier):
        try:
            self.__getitem__ (stream_identifier)
            return True
        except KeyError:
            return False

    def __delitem__(self, stream_identifier):
        self[stream_identifier].unregister ()

    def keys(self):
        return [s.cookie for s in self.manager.list_streams ()]

    def __len__(self):
        return len (self.manager.list_streams ())

    def available(self):
        """
        :returns: Whether the Woodchuck daemon is available.

        If the Woodchuck daemon is not available, all other methods
        will raise a :exc:`woodchuck.WoodchuckUnavailableError`.

        Note:: Unlike nearly all other functions in pywoodchuck, this
            function is thread safe.

        Example::

            import pywoodchuck

            w = pywoodchuck.PyWoodchuck("RSS Reader", "org.rssreader")
            if not w.available ():
                print "Woodchuck functionality not available."
            else:
                print "Woodchuck functionality available."
        """
        return self.manager is not None

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
            update the stream.  A value of
            :data:`woodchuck.never_updated` is interpretted as meaning
            that the stream is never updated and
            :func:`PyWoodchuck.stream_update_cb` will never be called.

        :returns: Returns a :class:`_Stream` instance.

        Example::

            import pywoodchuck
            import woodchuck

            w = pywoodchuck.PyWoodchuck("RSS Reader", "org.rssreader")

            w.stream_register("http://feeds.boingboing.net/boingboing/iBag",
                              "BoingBoing")
            try:
                w.stream_register("http://feeds.boingboing.net/boingboing/iBag",
                                  "BoingBoing")
            except woodchuck.ObjectExistsError as exception:
                print "Stream already registered:", exception

            del w["http://feeds.boingboing.net/boingboing/iBag"]
        """
        self.manager.stream_register(
            only_if_cookie_unique=True,
            human_readable_name=human_readable_name,
            cookie=stream_identifier,
            freshness=freshness)

        return self[stream_identifier]

    def streams_list(self):
        """
        List all streams managed by this application.

        :returns: Returns a list of :class:`_Stream` instances.

        Example::

            import pywoodchuck

            w = pywoodchuck.PyWoodchuck("Application", "org.application")
            w.stream_register("id:foo", "Foo")
            w.stream_register("id:bar", "Bar")
            for s in w.streams_list ():
                print "%s: %s" % (s.human_readable_name, s.identifier)
                del w[s.identifier]

        .. Note:: This is equivalent to iterating over the PyWoodchuck
            instance::

                import pywoodchuck

                w = pywoodchuck.PyWoodchuck("Application", "org.application")
                w.stream_register("id:foo", "Foo")
                w.stream_register("id:bar", "Bar")
                for s in w.values ():
                    print "%s: %s" % (s.human_readable_name, s.identifier)
                    del w[s.identifier]
        """
        return self.values()

    def stream_unregister(self, stream_identifier):
        """
        Unregister the indicated stream and any objects in contains.

        .. Note:: This function is an alias for
            :func:`_Stream.unregister`::

                pywoodchuck[stream_identifier].unregister()

            It is also equivalent to using the del operator except
            instead of raising :exc:`woodchuck.NoSuchObject`, del
            raises :exc:`KeyError` if the object does not exist::

                del pywoodchuck[stream_identifier].
        """
        try:
            del self[stream_identifier]
        except KeyError, exception:
            raise woodchuck.NoSuchObject (exception.args[0])

    def stream_updated(self, stream_identifier, *args, **kwargs):
        """
        Tell Woodchuck that a stream has been successfully updated.

        .. Note::

            This function is an alias for :func:`_Stream.updated`::

                pywoodchuck[stream_identifier].updated(...)

        :param stream_identifier: The stream's identifier.

        The remaining parameters are passed through to
        :func:`_Stream.updated`.
        """
        self[stream_identifier].updated (*args, **kwargs)

    def stream_update_failed(self, stream_identifier, *args, **kwargs):
        """
        Tell Woodchuck that a stream update failed.

        :param stream_identifier: The stream's identifier.

        .. Note::

            This function is an alias for
            :func:`_Stream.update_failed`::

                pywoodchuck[stream_identifier].update_failed(...)

        The remaining parameters are passed through to
        :func:`_Stream.update_failed`.
        """
        self[stream_identifier].update_failed(*args, **kwargs)

    def object_register(self, stream_identifier, *args, **kwargs):
        """
        Register an object.

        .. Note::

            This function is an alias for
            :func:`_Stream.object_register`::

                pywoodchuck[stream_identifier].object_register (...)

        :param stream_identifier: The stream's identifier.

        The remaining parameters are passed through to
        :func:`_Stream.object_register`.
        """
        return self[stream_identifier].object_register (*args, **kwargs)

    def objects_list(self, stream_identifier):
        """
        List the objects in a stream.

        .. Note::

            This function is an alias for
            :func:`_Stream.objects_list`::

                pywoodchuck[stream_identifier].objects_list (...)

            And for iterating over a :class:`_Stream` object:

                for obj in pywoodchuck[stream_identifier].values (): pass

        :param stream_identifier: The stream's identifier.
        """
        return self[stream_identifier].objects_list ()

    def object_transferred(self, stream_identifier, object_identifier,
                           *args, **kwargs):
        """
        Tell Woodchuck that an object was successfully transferred.

        .. Note::

            This function is an alias for
            :func:`_Stream.object_transferred`::

                pywoodchuck[stream_identifier].object_transferred (...)

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.

        The remaining parameters are passed through to
        :func:`_Stream.object_transferred`.
        """
        return self[stream_identifier].object_transferred (
            object_identifier, *args, **kwargs)

    def object_transfer_failed(self, stream_identifier, object_identifier,
                               *args, **kwargs):
        """
        Indicate that the program failed to transfer the object.

        .. Note::

            This function is an alias for
            :func:`_Stream.object_transfer_failed`::

                pywoodchuck[stream_identifier].object_transfer_failed (...)

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.

        The remaining parameters are passed through to
        :func:`_Stream.object_transfer_failed`.
        """
        return self[stream_identifier].object_transfer_failed (
            object_identifier, *args, **kwargs)

    def object_used(self, stream_identifier, object_identifier,
                    *args, **kwargs):
        """
        Indicate that the object has been used.

        .. Note::

            This function is an alias for :func:`_Object.used`::

                pywoodchuck[stream_identifier][object_identifier].used (...)

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.

        The remaining parameters are passed through to
        :func:`_Object.used`.
        """
        return self[stream_identifier][object_identifier].used (
            *args, **kwargs)

    def object_files_deleted(self, stream_identifier, object_identifier,
                             *args, **kwargs):
        """
        Indicate that the files associated with the object have been
        deleted, compressed (e.g., an email attachment, but not the
        body, was deleted) or that a deletion request has been vetoed,
        because, e.g., the application thinks the user still needs the
        data.

        .. Note::

            This function is an alias for
            :func:`_Object.files_deleted`::

                pywoodchuck[stream_identifier][object_identifier].files_deleted (...)

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.

        The remaining parameters are passed through to
        :func:`_Object.files_deleted`.
        """
        return self[stream_identifier][object_identifier].files_deleted (
            *args, **kwargs)

    def object_unregister(self, stream_identifier, object_identifier):
        """
        Unregister an object.

        .. Note::

            This function is an alias for :func:`_Object.unregister`::

                pywoodchuck[stream_identifier][object_identifier].unregister ()

        :param stream_identifier: The stream's identifier.

        :param object_identifier: The object's identifier.
        """
        return self[stream_identifier][object_identifier].unregister ()

    def stream_property_get(self, stream_identifier, property):
        """
        Get a stream's property.

        :param stream_identifier: The stream's identifier.

        :param property: A property, e.g., freshness.

        See :func:`stream_property_set` for an example use of this function.
        """
        return self[stream_identifier].__getattribute__(property)

    def stream_property_set(self, stream_identifier, property, value):
        """
        Set a stream's property.

        :param stream_identifier: The stream's identifier.

        :param property: A property, e.g., freshness.

        :param value: The new value.

        Example::

            import pywoodchuck
            import woodchuck

            w = pywoodchuck.PyWoodchuck("HMail", "org.hmail")
            w.stream_register("user@provider.com/INBOX", "Provider Inbox",
                              freshness=30*60)

            print w.stream_property_get ("user@provider.com/INBOX", "freshness")
            w.stream_property_set ("user@provider.com/INBOX",
                                   "freshness", 15*60)
            print w.stream_property_get ("user@provider.com/INBOX", "freshness")

        .. Note:: Properties can also be get and set by accessing the
            equivalently named attributes.  Thus, the above code could
            be rewritten as follows::

                import pywoodchuck
                import woodchuck
    
                w = pywoodchuck.PyWoodchuck("HMail", "org.hmail")
                w.stream_register("user@provider.com/INBOX", "Provider Inbox",
                                  freshness=30*60)
    
                print["user@provider.com/INBOX].freshness
                w["user@provider.com/INBOX"].freshness = 15*60
                print w["user@provider.com/INBOX"].freshness
        """
        return self[stream_identifier].__setattr__(property, value)

    def object_property_get(self, stream_identifier, object_identifier,
                            property):
        """
        Get an object's property.

        :param stream_identifier: The stream identifier.

        :param object_identifier: The object's identifier.

        :param property: A property, e.g., publication_time.

        See :func:`stream_property_set` for an example use of a
        similar function.
        """
        return (self[stream_identifier][object_identifier].
                __getattribute__(property))

    def object_property_set(self, stream_identifier, object_identifier,
                            property, value):
        """
        Set an object's property.

        :param stream_identifier: The stream identifier.

        :param object_identifier: The object's identifier.

        :param property: A property, e.g., publication_time.

        :param value: The new value.

        See :func:`stream_property_set` for an example use of a
        similar function.
        """
        return (self[stream_identifier][object_identifier].
                __setattr__(property, value))


    def object_transferred_cb(self, stream, object,
                             status, instance, version,
                             filename, size, trigger_target, trigger_fired):
        """Virtual method that should be implemented by the child
        class if it is interested in receiving object transferred
        notifications (:func:`org.woodchuck.upcall.ObjectTransferred`).

        This upcall is invoked when Woodchuck transfers an object on
        behalf of a manager.  This is only done for objects using the
        simple transferer.

        .. note: Woodchuck only transfers objects if it is explicitly
            told to by way of the `use_simple_transferer` property.
            See the `versions` parameter to :func:`object_register`
            for more details.

            If you don't use this option, there is no need to
            implement this method.

        :param stream: The stream, an instance of :class:`_Stream`.

        :param object: The object, an instance of :class:`_Object`.

        :param status: Whether the transfer was successfully.  The
            value is taken from :class:`woodchuck.TransferStatus`.

        :param instance: The number of transfer attempts (not
            including this one).

        :param version: The version that was transferred.  An array of:
            the index in the version array, the URL, the expected
            size, the expected bytes uploaded, expected bytes
            transferred, the utility and the value of use simple
            transferer.

        :param filename: The name of the file containing the data.

        :param size: The size of the file, in bytes.

        :param trigger_target: The time the application requested the
            object be transferred.

        :param trigger_fired: The time at which the file was actually
            transferred.

        Example: for an example of how to implement an upcall, see the
        opening example to :class:`PyWoodchuck`.
        """
        pass

    def stream_update_cb(self, stream):
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

        :param stream: The stream, an instance of :class:`_Stream`.

        Example: for an example of how to implement an upcall, see the
        opening example to :class:`PyWoodchuck`.
        """
        pass
    
    def object_transfer_cb(self, stream, object,
                           version, filename, quality):
        """
        Virtual method that should be implemented by the child class
        if it is interested in receiving object transfer notifications
        (:func:`org.woodchuck.upcall.ObjectTransfer`).

        This upcall is invoked when an object should be transferred.
        The application should transfer the object and call either
        :func:`object_transferred` or :func:`object_transfer_failed`,
        as appropriate.

        .. note: If all objects are marked as using the simple
            transferer (see the `versions` parameter to
            :func:`object_register` for more details), there is no
            need to implement this upcall.

        :param stream: The stream, an instance of :class:`_Stream`.

        :param object: The object, an instance of :class:`_Object`.

        :param version: The version to transfer.  An array of: the
            index in the version array, the URL, the expected size,
            the expected bytes uploaded, expected bytes transferred,
            the utility and the value of use simple transferer.

        :param filename: The name of the filename property.

        :param quality: The degree to which quality should be
            sacrified to reduce the number of bytes transferred.  The
            target quality of the transfer.  From 1 (most compressed)
            to 5 (highest available fidelity).

        Example: for an example of how to implement an upcall, see the
        opening example to :class:`PyWoodchuck`.
        """
        pass
    
    def object_delete_files_cb(self, stream, object):
        """Virtual method that should be implemented by the child
        class if it is interested in receiving deletion requests
        (:func:`org.woodchuck.upcall.ObjectDeleteFiles`).

        This upcall is invoked when an object's files should be
        transferred.  The application should respond with
        :func:`object_files_deleted`.

        .. note: If no objects are marked having the deletion policy
            :data:woodchuck.DeletionPolicy.DeleteWithConsultation`,
            there is no need to implement this upcall.

        :param stream: The stream, an instance of :class:`_Stream`.

        :param object: The object, an instance of :class:`_Object`.

        Example: for an example of how to implement an upcall, see the
        opening example to :class:`PyWoodchuck`.
        """
        pass
    

if __name__ == "__main__":
    import glib
    from dbus.mainloop.glib import DBusGMainLoop
    DBusGMainLoop(set_as_default=True)

    class WoodyChucky (PyWoodchuck):
        def stream_update_cb(self, stream):
            print "stream update called on %s" % (stream.identifier,)
        def object_transfer_cb(self, stream, obj,
                               version, filename, quality):
            print "object transfer called on %s in stream %s" \
                % (obj.identifier, stream.identifier);

    wc = WoodyChucky ("PyWoodchuck Test.", "org.woodchuck.pywoodchuck.test")
    if not wc.available ():
        print "Woodchuck server not available.  Unable to run test suite."
        import sys
        sys.exit (1)

    try:
        wc.stream_register('id:a', 'A', freshness=2)
        wc.stream_register('id:b', 'B', freshness=60*60)
        try:
            failed = False
            wc.stream_register('id:b', 'B')
        except woodchuck.ObjectExistsError, exception:
            failed = True
        assert failed

        wc.stream_unregister('id:a')
        wc.stream_unregister('id:b')
        try:
            failed = False
            wc.stream_unregister('id:b')
        except woodchuck.NoSuchObject, exception:
            failed = True
        assert failed

        assert len (wc) == 0

        wc.stream_register('id:a', 'A', freshness=3)
        assert wc.stream_property_get('id:a', 'freshness') == 3
        wc.stream_property_set('id:a', 'freshness', 2)
        assert wc.stream_property_get('id:a', 'freshness') == 2

        wc.stream_register('id:b', 'B', freshness=60*60)
        assert wc.stream_property_get('id:b', 'freshness') == 60*60

        assert len (wc) == 2

        print "Waiting for 5 seconds for stream feedback."
        loop = glib.MainLoop ()
        glib.timeout_add_seconds (5, glib.MainLoop.quit, loop)
        loop.run ()
        print "Mainloop returned."

        wc.stream_updated('id:a', transfer_duration=1,
                          new_objects=3, objects_inline=3)
        wc.stream_updated('id:b', transfer_duration=1,
                          new_objects=3, objects_inline=3)

        for i in (1, 2, 3):
            wc.object_register('id:a', 'id:a.' + str(i), 'A.' + str(i),
                               transfer_frequency=3,
                               versions=(("", 100, 100, 2, 1, False),
                                         ("", 20, 20, 2, 0, False)))
            assert wc.object_property_get('id:a', 'id:a.' + str(i),
                                          'transfer_frequency') == 3
            wc.object_property_set('id:a', 'id:a.' + str(i),
                                   'transfer_frequency', 2)
            assert wc.object_property_get('id:a', 'id:a.' + str(i),
                                          'transfer_frequency') == 2

            print "Waiting for 5 seconds for object feedback."
            loop = glib.MainLoop ()
            glib.timeout_add_seconds (5, glib.MainLoop.quit, loop)
            loop.run ()
            print "Mainloop returned."

            if i == 3:
                wc.object_transfer_failed ('id:a', 'id:a.' + str(i),
                                           woodchuck.TransferStatus.FailureGone)
            else:
                wc.object_transferred('id:a', 'id:a.' + str(i),
                                      object_size=(100 * i))

            wc.object_transferred('id:b', 'id:b.' + str(i),
                                  object_size=(1000 * i))

            wc.object_used ('id:b', 'id:b.' + str(i), use_mask=0x1)

        assert len (wc['id:a']) == 3
        assert len (wc['id:b']) == 3

        for i in (1, 2, 3):
            wc.object_files_deleted ('id:a', 'id:a.' + str(i))
            wc.object_unregister('id:a', 'id:a.' + str(i))
            assert len (wc['id:a']) == 3 - i

        wc.stream_update_failed ('id:a',
                                 woodchuck.TransferStatus.TransientNetwork)
        wc.stream_update_failed ('id:b',
                                 woodchuck.TransferStatus.FailureGone,
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
