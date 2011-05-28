#! /bin/python

# woodchuck.py - A low-level woodchuck module for Python.
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

import dbus
import dbus.service
import time

"""A low-level wrapper of the org.woodchuck DBus interfaces."""

class DownloadStatus:
    """Values for the Indicator argument of
    org.woodchuck.object.DownloadStatus and
    org.woodchuck.stream.StreamUpdated."""
    Success = 0

    TransientOther = 0x100
    TransientNetwork = 0x101
    TransientInterrupted = 0x102

    FailureOther = 0x200
    FailureGone = 0x201

class Indicator:
    """Values for the Indicator argument of
    org.woodchuck.object.DownloadStatus and
    org.woodchuck.stream.StreamUpdated."""
    Audio = 0x1
    ApplicationVisual = 0x2
    DesktopSmallVisual = 0x4
    DesktopLargeVisual = 0x8
    ExternalVisual = 0x10
    Vibrate = 0x20

    ObjectSpecific = 0x40
    SystemWide = 0x80
    ManagerWide = 0x100

    Unknown = 0x80000000

class DeletionPolicy:
    """Values for the DELETION_POLICY argument of
    org.woodchuck.object.DownloadStatus."""
    Precious = 0
    DeleteWithoutConsultation = 1
    DeleteWithConsultation = 2

class DeletionResponse:
    """Values for the Update arguments of
    org.woodchuck.object.FilesDeleted."""
    Deleted = 0
    Refused = 1
    Compressed = 2

class Error(Exception):
    """Base class for exceptions in this model.  args[0] contains a
    more detailed description of the error."""
    pass

class GenericError(Error): pass
class NoSuchObject(Error): pass
class ObjectExistsError(Error): pass
class NotImplementedError(Error): pass
class InternalError(Error): pass
class InvalidArgsError(Error): pass
class UnknownError(Error): pass

def _dbus_exception_to_woodchuck_exception(exception):
    """Convert a dbus exception to a local exception."""
    if exception.get_dbus_name () == "org.woodchuck.GenericError":
        raise GenericError (exception.get_dbus_message ())
    elif exception.get_dbus_name () == "org.woodchuck.ObjectExists":
        raise ObjectExistsError (exception.get_dbus_message ())
    elif exception.get_dbus_name () == "org.woodchuck.MethodNotImplemented":
        raise NotImplementedError (exception.get_dbus_message ())
    elif exception.get_dbus_name () == "org.woodchuck.InternalError":
        raise InternalError (exception.get_dbus_message ())
    elif exception.get_dbus_name () == "org.woodchuck.InvalidArgs":
        raise InvalidArgsError (exception.get_dbus_message ())
    elif exception.get_dbus_name ().startswith ("org.woodchuck."):
        raise UnknownError (exception.get_dbus_name ()
                            + exception.get_dbus_message ())
    else:
        raise exception

_manager_properties_to_camel_case = \
    dict ({"UUID": ("UUID", dbus.UTF8String, ""),
           "parent_UUID": ("ParentUUID", dbus.UTF8String, ""),
           "human_readable_name": ("HumanReadableName", dbus.UTF8String, ""),
           "cookie": ("Cookie", dbus.UTF8String, ""),
           "dbus_service_name": ("DBusServiceName", dbus.UTF8String, ""),
           "dbus_object": ("DBusObject", dbus.UTF8String, ""),
           "priority": ("Priority", dbus.UInt32, 0),
          })
_manager_properties_from_camel_case = \
    dict([[k2, (k, t, d)] for k, (k2, t, d)
          in _manager_properties_to_camel_case.items ()])

_stream_properties_to_camel_case = \
    dict ({"UUID": ("UUID", dbus.UTF8String, ""),
           "parent_UUID": ("ParentUUID", dbus.UTF8String, ""),
           "human_readable_name": ("HumanReadableName", dbus.UTF8String, ""),
           "cookie": ("Cookie", dbus.UTF8String, ""),
           "priority": ("Priority", dbus.UInt32, 0),
           "freshness": ("Freshness", dbus.UInt32, 0),
           "object_mostly_inline": ("ObjectsMostlyInline", dbus.Boolean, False),
           })
_stream_properties_from_camel_case = \
    dict([[k2, (k, t, d)] for k, (k2, t, d)
          in _stream_properties_to_camel_case.items ()])

_object_properties_to_camel_case = \
    dict ({"UUID": ("UUID", dbus.UTF8String, ""),
           "parent_UUID": ("ParentUUID", dbus.UTF8String, ""),
           "instance": ("Instance", dbus.UInt32, 0),
           "human_readable_name": ("HumanReadableName", dbus.UTF8String, ""),
           "cookie": ("Cookie", dbus.UTF8String, ""),
           "versions": ("Versions", lambda v: dbus.Array (v, "(stub)"), []),
           "filename": ("Filename", dbus.UTF8String, ""),
           "wakeup": ("Wakeup", dbus.Boolean, True),
           "trigger_target": ("TriggerTarget", dbus.UInt64, 0),
           "trigger_earliest": ("TriggerEarliest", dbus.UInt64, 0),
           "trigger_latest": ("TriggerLatest", dbus.UInt64, 0),
           "download_frequency": ("DownloadFrequency", dbus.UInt32, 0),
           "priority": ("Priority", dbus.UInt32, 0),
           })
_object_properties_from_camel_case = \
    dict([[k2, (k, t, d)] for k, (k2, t, d)
          in _object_properties_to_camel_case.items ()])

def _keys_convert(d, conversion):
    """Convert properties from the python domain to the DBus domain
    (or vice versa).  This includes converting the property names from
    lower_case (the preferred identifier format according to PEP8) to
    CamelCase (the preferred identifier format according to the DBus
    specification), setting the values type and using a default is the
    value is None.

    Return a dictionary in which the keys of D are mapped according to
    the map CONVERSION.

    For example, if d={'a':1, 'b':None} and
    conversion={'a':('A', dbus.UInt, 0), 'b':('B', dbus.UInt, 0)}, then
    this function returns the dictionary {'A':1, 'B':0}."""
    return dict ([[conversion[k][0],
                   conversion[k][1] (v if v is not None
                                     else conversion[k][1][2])]
                  for k, v in d.items ()])

class _Object():
    """The local representation for a Woodchuck object."""
    def __init__(self, **properties):
        """ You should never instantiate this class yourself.
        Instead, use a method that returns an object, such as
        Stream.LookupObjectByCookie.

        The UUID property is required."""

        # Check the properties.
        for k in properties.keys ():
            assert k in _object_properties_to_camel_case

        self.properties = properties
        self.proxy = dbus.SessionBus().get_object ('org.woodchuck',
                                                   '/org/woodchuck/object/'
                                                   + self.properties['UUID'])
        self.dbus = dbus.Interface (self.proxy,
                                    dbus_interface='org.woodchuck.object')

    def __repr__(self):
        return self.properties.__repr__ ()
    def __str__(self):
        return self.__repr__ ().__str__ ()

    def __getattr__(self, name):
        if name in self.properties:
            return self.properties[name]
        else:
            raise AttributeError (("%s instance has no attribute '%s'"
                                   % (self.__class__.__name__, name)))

    def unregister(self):
        try:
            ret = self.dbus.Unregister ()
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

        if ret:
            del _objects[self.properties['UUID']]

    def download(self, request_type):
        try:
            self.dbus.Download (request_type)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

    def download_status(self, status, indicator=None,
                        transferred_up=None, transferred_down=None,
                        download_time=None, download_duration=None,
                        object_size=None, files=None):
        if indicator is None:
            indicator = 0x80000000
        indicator = dbus.UInt32 (indicator)

        if transferred_up is None:
            transferred_up = 2 ** 64 - 1
        transferred_up = dbus.UInt64 (transferred_up)

        if transferred_down is None:
            transferred_down = 2 ** 64 - 1
        transferred_down = dbus.UInt64 (transferred_down)

        if download_time is None:
            download_time = int (time.time ())
        download_time = dbus.UInt64 (download_time)

        if download_duration is None:
            download_duration = 0
        download_duration = dbus.UInt32 (download_duration)

        if object_size is None:
            object_size = 2 ** 64 - 1
        object_size = dbus.UInt64 (object_size)

        files = dbus.Array (file if files is not None else [], "(stu)")

        try:
            self.dbus.DownloadStatus(status, indicator,
                                     transferred_up, transferred_down,
                                     download_time, download_duration,
                                     object_size, files)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

    def used(self, start=None, duration=None, use_mask=None):
        if start is None:
            start = int (time.time ())
        if duration is None:
            duration = 2 ** 64 - 1
        if use_mask is None:
            use_mask = 2 ** 64 - 1

        try:
            self.dbus.Used(start, duration, use_mask)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

    def files_deleted(self, update=None, arg=None):
        if update is None or update == DeletionResponse.Deleted:
            update = DeletionResponse.Deleted
            arg = 0
        elif arg is None:
            if update == DeletionResponse.Refused:
                arg = int (time.time ()) + 5 * 60
            elif update == DeletionResponse.Compressed:
                arg = 2 ** 64 - 1
        
        try:
            self.dbus.FilesDeleted(update, arg)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

_objects = {}
def Object(**properties):
    """Instantiate a local object."""
    if properties['UUID'] in _objects:
        return _objects[properties['UUID']]
    return _Object(**properties)

class _Stream():
    def __init__(self, **properties):
        """ You should never instantiate this class yourself.
        Instead, use a method that returns an object, such as
        Stream.LookupObjectByCookie.

        The UUID property is required."""

        # Check the properties.
        for k in properties.keys ():
            assert k in _stream_properties_to_camel_case

        self.properties = properties
        self.proxy = dbus.SessionBus().get_object ('org.woodchuck',
                                                   '/org/woodchuck/stream/'
                                                   + self.properties['UUID'])
        try:
            self.dbus = dbus.Interface (self.proxy,
                                        dbus_interface='org.woodchuck.stream')
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

    def __repr__(self):
        return self.properties
    def __str__(self):
        return self.__repr__ ().__str__ ()

    def __getattr__(self, name):
        if name in self.properties:
            return self.properties[name]
        else:
            raise AttributeError (("%s instance has no attribute '%s'"
                                   % (self.__class__.__name__, name)))

    def unregister(self, only_if_empty):
        try:
            ret = self.dbus.Unregister (only_if_empty)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

        if ret:
            del _streams[self.properties['UUID']]

    def object_register(self, only_if_cookie_unique=True, **properties):
        try:
            UUID = self.dbus.ObjectRegister \
                (_keys_convert (properties, _object_properties_to_camel_case),
                 only_if_cookie_unique)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

        properties['UUID'] = UUID
        return Object (**properties)

    def list_objects(self):
        try:
            return [Object(UUID=UUID, human_readable_name=human_readable_name,
                           cookie=cookie, parent_UUID=self.properties['UUID'])
                    for UUID, cookie, human_readable_name
                    in self.dbus.ListObjects ()]
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

    def lookup_object_by_cookie(self, cookie):
        try:
            return [Object(UUID=UUID, human_readable_name=human_readable_name,
                           cookie=cookie, parent_UUID=self.properties['UUID'])
                    for UUID, human_readable_name
                    in self.dbus.LookupObjectByCookie (cookie)]
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

    def update_status(self, status, indicator=None,
                      transferred_up=None, transferred_down=None,
                      download_time=None, download_duration=None,
                      new_objects=None, updated_objects=None,
                      objects_inline=None):
        if indicator is None:
            indicator = 0
        if transferred_up is None:
            transferred_up = 2 ** 64 - 1
        if transferred_down is None:
            transferred_down = 2 ** 64 - 1
        if download_time is None:
            download_time = int (time.time ())
        if download_duration is None:
            download_duration = 0
        if new_objects is None:
            new_objects = 2 ** 32 - 1
        if updated_objects is None:
            updated_objects = 2 ** 32 - 1
        if objects_inline is None:
            objects_inline = 2 ** 32 - 1

        try:
            self.dbus.UpdateStatus(status, indicator,
                                   transferred_up, transferred_down,
                                   download_time, download_duration,
                                   new_objects, updated_objects, objects_inline)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

_streams = {}
def Stream(**properties):
    """Instantiate a local stream object."""
    if properties['UUID'] in _streams:
        return _streams[properties['UUID']]
    return _Stream(**properties)

class _Manager():
    def __init__(self, **properties):
        """ You should never instantiate this class yourself.
        Instead, use a method that returns an object, such as
        Stream.LookupObjectByCookie.

        The UUID property is required."""

        # Check the properties.
        for k in properties.keys ():
            assert k in _manager_properties_to_camel_case

        self.properties = properties
        self.proxy = dbus.SessionBus().get_object ('org.woodchuck',
                                                   '/org/woodchuck/manager/'
                                                   + self.properties['UUID'])
        self.dbus = dbus.Interface (self.proxy,
                                    dbus_interface='org.woodchuck.manager')

        self.feedback_subscriptions = 0
        self.feedback_subscription_handle = None

    def __repr__(self):
        return self.properties.__repr__ ()
    def __str__(self):
        return self.__repr__ ().__str__ ()

    def __getattr__(self, name):
        if name in self.properties:
            return self.properties[name]
        else:
            raise AttributeError (("%s instance has no attribute '%s'"
                                   % (self.__class__.__name__, name)))

    def unregister(self, only_if_empty=True):
        try:
            ret = self.dbus.Unregister (only_if_empty)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)
        if ret:
            del _managers[self.properties['UUID']]

    def manager_register(self, only_if_cookie_unique=True,
                         **properties):
        try:
            UUID = self.dbus.ManagerRegister \
                (_keys_convert (properties, _manager_properties_to_camel_case),
                 only_if_cookie_unique)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

        properties['UUID'] = UUID
        return Manager (**properties)

    def list_managers(self, recursive=False):
        try:
            return [Manager(UUID=UUID, human_readable_name=human_readable_name,
                            cookie=cookie, parent_UUID=parent_UUID)
                    for UUID, cookie, human_readable_name, parent_UUID
                    in self.dbus.ListManagers (recursive)]
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

    def lookup_manager_by_cookie(self, cookie, recursive=False):
        try:
            return [Manager(UUID=UUID, human_readable_name=human_readable_name,
                            cookie=cookie, parent_UUID=parent_UUID)
                    for UUID, human_readable_name, parent_UUID
                    in self.dbus.LookupManagerByCookie (cookie, recursive)]
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

    def stream_register(self, only_if_cookie_unique=True, **properties):
        try:
            UUID = self.dbus.StreamRegister \
                (_keys_convert (properties, _stream_properties_to_camel_case),
                 only_if_cookie_unique)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

        properties['UUID'] = UUID
        return Stream (**properties)

    def list_streams(self):
        try:
            return [Stream(UUID=UUID, human_readable_name=human_readable_name,
                           cookie=cookie, parent_UUID=self.properties['UUID'])
                    for UUID, cookie, human_readable_name
                    in self.dbus.ListStreams ()]
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

    def lookup_stream_by_cookie(self, cookie):
        try:
            return [Stream(UUID=UUID, human_readable_name=human_readable_name,
                           cookie=cookie, parent_UUID=self.properties['UUID'])
                    for UUID, human_readable_name
                    in self.dbus.LookupStreamByCookie (cookie)]
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

    def feedback_subscribe(self, descendents_too=True):
        if self.feedback_subscription_handle is not None:
            # Already subscribed.
            print "Already subscribed..."
            self.feedback_subscriptions += 1
            return

        self.feedback_subscription_handle \
            = self.dbus.FeedbackSubscribe (descendents_too)

    def feedback_unsubscribe(self):
        if self.feedback_subscriptions == 0:
            raise Foo;
        else:
            self.feedback_subscriptions -= 1
            if self.feedback_subscriptions == 0:
                try:
                    self.dbus.FeedbackAck (self.feedback_subscription_handle)
                except dbus.exceptions.DBusException as exception:
                    _dbus_exception_to_woodchuck_exception (exception)
                self.feedback_subscription_handle = None

    def feedback_ack(self, object_UUID, object_instance):
        try:
            self.dbus.FeedbackAck (object_UUID, object_instance)
        except dbus.exceptions.DBusException as exception:
            _dbus_exception_to_woodchuck_exception (exception)

# Establish a connection with the Woodchuck server.
_woodchuck_object = None
_woodchuck_server = None

def _woodchuck():
    global _woodchuck_object
    global _woodchuck_server
    if _woodchuck_object is None:
        _woodchuck_object = dbus.SessionBus().get_object ('org.woodchuck',
                                                          '/org/woodchuck')
        _woodchuck_server = dbus.Interface (_woodchuck_object,
                                            dbus_interface='org.woodchuck')
    return _woodchuck_server

_managers = {}
def Manager(**properties):
    """Instantiate a local manager object."""
    if properties['UUID'] in _managers:
        return _managers[properties['UUID']]
    return _Manager(**properties)

def list_managers(recursive=False):
    try:
        return [Manager(UUID=UUID, human_readable_name=human_readable_name,
                        cookie=cookie, parent_UUID=parent_UUID)
                for UUID, cookie, human_readable_name, parent_UUID
                in _woodchuck().ListManagers (recursive)]
    except dbus.exceptions.DBusException as exception:
        _dbus_exception_to_woodchuck_exception (exception)

def manager_register(only_if_cookie_unique=True, **properties):
    assert 'parent_UUID' not in properties
    try:
        UUID = _woodchuck().ManagerRegister \
            (_keys_convert (properties, _manager_properties_to_camel_case),
             only_if_cookie_unique)
    except dbus.exceptions.DBusException as exception:
        _dbus_exception_to_woodchuck_exception (exception)

    properties['UUID'] = UUID
    return Manager (**properties)

def lookup_manager_by_cookie(cookie, recursive=False):
    try:
        return [Manager(UUID=UUID, human_readable_name=human_readable_name,
                        cookie=cookie, parent_UUID=parent_UUID)
                for UUID, human_readable_name, parent_UUID
                in _woodchuck().LookupManagerByCookie (cookie, recursive)]
    except dbus.exceptions.DBusException as exception:
        _dbus_exception_to_woodchuck_exception (exception)

# As there is only a single woodchuck instance, tracking the Woodchuck
# instance and checking whether messages comes from it can be shared
# by all instances of Upcalls.
_watching_woodchuck_owner = False
_woodchuck_owner = None

def _woodchuck_owner_update(new_owner):
    """DBus watch name owner call back.  (Initialized the first time
    the Upcalls class is instantiated.)"""
    global _woodchuck_owner
    _woodchuck_owner = new_owner

def _is_woodchuck(name):
    """Return whether NAME is the private dbus name of the woodchuck
    daemon."""
    if name[0] != ':':
        raise ValueError(("%s is not a private DBus name "
                          "(should start with ':')") % (name,))

    if name != _woodchuck_owner:
        print ("Message from %s, expected message from %s" \
                   % (name, _woodchuck_owner))

    return name == _woodchuck_owner

class Upcalls(dbus.service.Object):
    """A thin wrapper around org.woodchuck.upcalls."""
    def __init__(self, path):
        """PATH is the object that will receive the upcalls from
        woodchuck.  Inherit from this class and implement the
        corresponding virtual methods."""
        bus = dbus.SessionBus()

        dbus.service.Object.__init__(self, bus, path)

        if not _watching_woodchuck_owner:
            bus.watch_name_owner ("org.woodchuck", _woodchuck_owner_update)
            _woodchuck_owner_update (bus.get_name_owner ("org.woodchuck"))

    @dbus.service.method(dbus_interface='org.woodchuck.upcall',
                         in_signature='ssssssuu(ustub)sttt',
                         out_signature='',
                         sender_keyword="sender")
    def ObjectDownloaded(self, manager_UUID, manager_cookie,
                         stream_UUID, stream_cookie,
                         object_UUID, object_cookie,
                         status, instance, version, filename, size,
                         trigger_target, trigger_fired, sender):
        if not _is_woodchuck (sender):
            return False

        return self.object_downloaded_cb(manager_UUID, manager_cookie,
                                         stream_UUID, stream_cookie,
                                         object_UUID, object_cookie,
                                         status, instance, version,
                                         filename, size,
                                         trigger_target, trigger_fired)

    def object_downloaded_cb(self, manager_UUID, manager_cookie,
                             stream_UUID, stream_cookie,
                             object_UUID, object_cookie,
                             status, instance, version, filename, size,
                             trigger_target, trigger_fired):
        """Virtual method that should be implemented by the child
        class if it is interested in
        org.woodchuck.upcall.ObjectDownloaded upcalls."""
        pass
    
    @dbus.service.method(dbus_interface='org.woodchuck.upcall',
                         in_signature='ssss', out_signature='',
                         sender_keyword="sender")
    def StreamUpdate(self, manager_UUID, manager_cookie,
                     stream_UUID, stream_cookie, sender):
        if not _is_woodchuck (sender):
            return False

        self.stream_update_cb (manager_UUID, manager_cookie,
                               stream_UUID, stream_cookie)

    def stream_update_cb(self, manager_UUID, manager_cookie,
                         stream_UUID, stream_cookie):
        """Virtual method that should be implemented by the child
        class if it is interested in
        org.woodchuck.upcall.StreamUpdate upcalls."""
        pass
    
    @dbus.service.method(dbus_interface='org.woodchuck.upcall',
                         in_signature='ssssss(ustub)su', out_signature='',
                         sender_keyword="sender")
    def ObjectDownload(self, manager_UUID, manager_cookie,
                       stream_UUID, stream_cookie,
                       object_UUID, object_cookie,
                       version, filename, quality, sender):
        if not _is_woodchuck (sender):
            return False

        self.object_download_cb (manager_UUID, manager_cookie,
                              stream_UUID, stream_cookie,
                              object_UUID, object_cookie,
                              version, filename, quality)

    def object_download_cb (self, manager_UUID, manager_cookie,
                            stream_UUID, stream_cookie,
                            object_UUID, object_cookie,
                            version, filename, quality):
        """Virtual method that should be implemented by the child
        class if it is interested in
        org.woodchuck.upcall.ObjectDownload upcalls."""
        pass

    @dbus.service.method(dbus_interface='org.woodchuck.upcall',
                         in_signature='ssssss', out_signature='',
                         sender_keyword="sender")
    def ObjectDeleteFiles(self, manager_UUID, manager_cookie,
                          stream_UUID, stream_cookie,
                          object_UUID, object_cookie, sender):
        if not _is_woodchuck (sender):
            return False

        self.object_delete_files_cb (manager_UUID, manager_cookie,
                                     stream_UUID, stream_cookie,
                                     object_UUID, object_cookie)

    def object_delete_files_cb (self, manager_UUID, manager_cookie,
                                stream_UUID, stream_cookie,
                                object_UUID, object_cookie):
        """Virtual method that should be implemented by the child
        class if it is interested in
        org.woodchuck.upcall.ObjectDeleteFiles upcalls."""
        pass

if __name__ == "__main__":
    import random
    cookie = str (random.random ())
    print list_managers()
    manager = manager_register (human_readable_name="Test", cookie=cookie,
                                only_if_cookie_unique=True)
    try:
        have_one = 0
        for m in lookup_manager_by_cookie (cookie, False):
            print "Considering: " + str (m)
            if m.properties['cookie'] == cookie:
                have_one += 1
        if have_one == 0:
            print "Failed to find manager we just registered!"
    finally:
        manager.unregister (True)
