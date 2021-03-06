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
# along with this program.  If not, see
# <http://www.gnu.org/licenses/>.

import dbus
import dbus.service
import time
import threading
from functools import wraps
import sys
import logging
logger = logging.getLogger(__name__)

#: Constant that can be passed to :func:`PyWoodchuck.stream_register`
#: indicating that the stream will never be updated.
never_updated=2 ** 32 - 1

"""A low-level wrapper of the org.woodchuck DBus interfaces."""

class RequestType:
    """Values for the request_type argument of
    :func:`_Object.transfer`."""

    #: The user initiated the transfer request.
    UserInitiated = 0
    #: The application initiated the transfer request.
    ApplicationInitiated = 0

class TransferStatus:
    """Values for the Indicator argument of
    :func:`woodchuck._Object.transfer_status`,
    :func:`woodchuck._Stream.update_status` and
    :func:`woodchuck.Upcalls.object_transferred_cb`.
    """
    #: The transfer was successful.
    Success = 0

    #: An unspecified transient error occurred.
    TransientOther = 0x100
    #: A transient network error occured, e.g., the host was
    #: unreachable.
    TransientNetwork = 0x101
    #: A transient error occured during the transfer.
    TransientInterrupted = 0x102

    #: An unspecified hard error occured.  Don't try again.
    FailureOther = 0x200
    #: A hard error, the object is gone, occured.
    FailureGone = 0x201

class Indicator:
    """Values for the Indicator argument of
    :func:`woodchuck._Object.transfer_status`,
    :func:`woodchuck._Stream.update_status`."""
    #: An audio sound was emitted.
    Audio = 0x1
    #: An visual notification was displayed in the application.
    ApplicationVisual = 0x2
    #: A small visual notification was displayed on the desktop, e.g.,
    #: in the system tray.
    DesktopSmallVisual = 0x4
    #: A large visual notification was displayed on the desktop.
    DesktopLargeVisual = 0x8
    #: An external visual notification was displayed, e.g., an LED was
    #: blinked.
    ExternalVisual = 0x10
    #: The device vibrated.
    Vibrate = 0x20

    #: The notification was object specific.
    ObjectSpecific = 0x40
    #: The notification was stream-wide, i.e., an aggregate
    #: notification for all updates in the stream.
    StreamWide = 0x80
    #: The notification was manager-wide, i.e., an aggregate :
    #: notification for multiple stream updates.
    ManagerWide = 0x100

    #: It is unknown whether an indicator was shown.
    Unknown = 0x80000000

class DeletionPolicy:
    """Values for the `deletion_policy` argument of
    :func:`woodchuck._Object.transfer_status`."""

    #: The file is precious and will only be deleted by the user.
    Precious = 0

    #: Woodchuck may delete the file without consulting the
    #: application.
    DeleteWithoutConsultation = 1

    #: Woodchuck may ask the application to delete the file.
    DeleteWithConsultation = 2

class DeletionResponse:
    """Values for the Update arguments of
    :func:`woodchuck._Object.files_deleted`"""
    #: The files associated with the object were deleted.
    Deleted = 0
    #: The application refuses to delete the object.
    Refused = 1
    #: The application compressed the object, e.g., for an email, it
    #discarded the attachments, but not the email's body.
    Compressed = 2

class Error(Exception):
    """Base class for exceptions in this model.  args[0] contains a
    more detailed description of the error."""

class GenericError(Error):
    """While invoking a Woodchuck method, a DBus error
    org.woodchuck.GenericError occured."""
class NoSuchObject(Error):
    """While invoking a Woodchuck method, a DBus error
    org.freedesktop.DBus.Error.UnknownObject occured."""
class ObjectExistsError(Error):
    """While invoking a Woodchuck method, a DBus error
    org.woodchuck.ObjectExists occured."""
class NotImplementedError(Error):
    """While invoking a Woodchuck method, a DBus error
    org.woodchuck.MethodNotImplemented occured."""
class InternalError(Error):
    """While invoking a Woodchuck method, a DBus error
    org.woodchuck.InternalError occured."""
class InvalidArgsError(Error):
    """While invoking a Woodchuck method, a DBus error
    org.woodchuck.InvalidArgs occured."""
class UnknownError(Error):
    """While invoking a Woodchuck method, an unknown DBus error with
    prefix org.woodchuck occured."""

class WoodchuckUnavailableError(Error):
    """The woodchuck server is unavailable.  For whatever reason, it
    couldn't be started.  This is a Python specific exception."""

def _dbus_exception_to_woodchuck_exception(exception):
    """Convert a dbus exception to a local exception."""
    try:
        dbus_name = str (exception.get_dbus_name ())
    except AttributeError:
        dbus_name = ""

    if dbus_name == "org.woodchuck.GenericError":
        raise GenericError (exception.get_dbus_message ())
    elif dbus_name == "org.woodchuck.ObjectExists":
        raise ObjectExistsError (exception.get_dbus_message ())
    elif dbus_name == "org.woodchuck.MethodNotImplemented":
        raise NotImplementedError (exception.get_dbus_message ())
    elif dbus_name == "org.woodchuck.InternalError":
        raise InternalError (exception.get_dbus_message ())
    elif dbus_name == "org.woodchuck.InvalidArgs":
        raise InvalidArgsError (exception.get_dbus_message ())
    elif dbus_name.startswith ("org.woodchuck."):
        raise UnknownError (dbus_name + exception.get_dbus_message ())
    elif (dbus_name == "org.freedesktop.DBus.Error.ServiceUnknown"
          or dbus_name == "org.freedesktop.DBus.Error.NoReply"):
        raise WoodchuckUnavailableError (exception.get_dbus_message ())
    else:
        raise exception

# The default amount of time to cache values.
_ttl = 1

def _str_to_dbus_str(s, strict=False):
    """
    Given a string, do our best to turn it into a unicode compatible
    object.
    """
    if issubclass(s.__class__, unicode):
        # It's already unicode, no problem.
        return s

    # It's not unicode.  Convert it to a unicode string.
    try:
        return unicode(s)
    except UnicodeEncodeError:
        logger.exception("Failed to convert '%s' to unicode" % s)
        if strict:
            raise
        else:
            return unicode(s, errors='replace')

def _str_to_dbus_str_strict(s):
    return _str_to_dbus_str(s, strict=True)

_manager_properties_to_camel_case = \
    dict ({"UUID": ("UUID", _str_to_dbus_str, "", float("inf")),
           "parent_UUID": ("ParentUUID", _str_to_dbus_str, "", float("inf")),
           "human_readable_name":
               ("HumanReadableName", _str_to_dbus_str, "", _ttl),
           "cookie": ("Cookie", _str_to_dbus_str_strict, "", _ttl),
           "dbus_service_name": ("DBusServiceName", _str_to_dbus_str, "", _ttl),
           "dbus_object": ("DBusObject", _str_to_dbus_str, "", _ttl),
           "priority": ("Priority", dbus.UInt32, 0, _ttl),
           "enabled": ("Enabled", dbus.Boolean, True, _ttl),
           "registration_time": ("RegistrationTime", dbus.UInt64, 0,
                                 float("inf")),
          })
_manager_properties_from_camel_case = \
    dict([[k2, (k, t, d, ttl)] for k, (k2, t, d, ttl)
          in _manager_properties_to_camel_case.items ()])

manager_properties=_manager_properties_to_camel_case.keys ()

_stream_properties_to_camel_case = \
    dict ({"UUID": ("UUID", _str_to_dbus_str, "", float("inf")),
           "parent_UUID": ("ParentUUID", _str_to_dbus_str, "", float("inf")),
           "human_readable_name":
               ("HumanReadableName", _str_to_dbus_str, "", _ttl),
           "cookie": ("Cookie", _str_to_dbus_str_strict, "", _ttl),
           "priority": ("Priority", dbus.UInt32, 0, _ttl),
           "freshness": ("Freshness", dbus.UInt32, 0, _ttl),
           "object_mostly_inline": ("ObjectsMostlyInline", dbus.Boolean,
                                    False, _ttl),
           "registration_time": ("RegistrationTime", dbus.UInt64, 0,
                                 float("inf")),
           "last_update_time":
               ("LastUpdateTime", dbus.UInt64, 0, _ttl),
           "last_update_attempt_time":
               ("LastUpdateAttemptTime", dbus.UInt64, 0, _ttl),
           "last_update_attempt_status":
               ("LastUpdateAttemptStatus", dbus.UInt32, 0, _ttl),
           })
_stream_properties_from_camel_case = \
    dict([[k2, (k, t, d, ttl)] for k, (k2, t, d, ttl)
          in _stream_properties_to_camel_case.items ()])

stream_properties=_stream_properties_to_camel_case.keys ()

_object_properties_to_camel_case = \
    dict ({"UUID": ("UUID", _str_to_dbus_str, "", float("inf")),
           "parent_UUID": ("ParentUUID", _str_to_dbus_str, "", float("inf")),
           "instance": ("Instance", dbus.UInt32, 0, _ttl),
           "human_readable_name":
               ("HumanReadableName", _str_to_dbus_str, "", _ttl),
           "cookie": ("Cookie", _str_to_dbus_str_strict, "", _ttl),
           "versions": ("Versions", lambda v: dbus.Array (v, "(sxttub)"), [],
                        _ttl),
           "filename": ("Filename", _str_to_dbus_str, "", _ttl),
           "wakeup": ("Wakeup", dbus.Boolean, True, _ttl),
           "trigger_target": ("TriggerTarget", dbus.UInt64, 0, _ttl),
           "trigger_earliest": ("TriggerEarliest", dbus.UInt64, 0, _ttl),
           "trigger_latest": ("TriggerLatest", dbus.UInt64, 0, _ttl),
           "transfer_frequency": ("TransferFrequency", dbus.UInt32, 0, _ttl),
           "dont_transfer": ("DontTransfer", dbus.Boolean, False, _ttl),
           "need_update": ("NeedUpdate", dbus.Boolean, True, _ttl),
           "priority": ("Priority", dbus.UInt32, 0, _ttl),
           "discovery_time": ("DiscoveryTime", dbus.UInt64, 0, _ttl),
           "publication_time": ("PublicationTime", dbus.UInt64, 0, _ttl),
           "registration_time": ("RegistrationTime", dbus.UInt64, 0,
                                 float("inf")),
           "last_transfer_time":
               ("LastTransferTime", dbus.UInt64, 0, _ttl),
           "last_transfer_attempt_time":
               ("LastTransferAttemptTime", dbus.UInt64, 0, _ttl),
           "last_transfer_attempt_status":
               ("LastTransferAttemptStatus", dbus.UInt32, 0, _ttl),
           })
_object_properties_from_camel_case = \
    dict([[k2, (k, t, d, ttl)] for k, (k2, t, d, ttl)
          in _object_properties_to_camel_case.items ()])

object_properties=_object_properties_to_camel_case.keys ()

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
                                     else conversion[k][2])]
                  for k, v in d.items()])

# The dbus library is not thread safe.  Ensure that all calls are
# executed in the same thread.
_main_thread = None
def _check_main_thread(f):
    @wraps(f)
    def wrapper(*args, **kwargs):
        global _main_thread
        if _main_thread is None:
            _main_thread = threading.currentThread()
        else:
            assert _main_thread == threading.currentThread(), \
                (("woodchuck (due to its use of DBus) is not thread-safe. "
                  + "Fix your code (main thread: %s; this thread: %s).")
                 % (str (_main_thread), str (threading.currentThread())))
        return f(*args, **kwargs)
    return wrapper

class DBusIndirectObject(object):
    """
    Wrap a DBus object.  Bind the server name lazily, i.e., if the
    server exits and appears under a different private name,
    transparently use that.
    """
    __slots__ = ('bus', 'args', 'kwargs', 'real_object')
    def __init__(self, bus, *args, **kwargs):
        self.bus = bus
        self.args = args
        self.kwargs = kwargs
        self.bind_object()

    def bind_object(self):
        self.real_object = self.bus.get_object(*self.args, **self.kwargs)

    def __getattr__(self, attr):
        if attr in self.__slots__:
            return super(DBusIndirectObject, self).__getattr__(attr)
        return getattr(self.real_object, attr)

    def __setattr__(self, attr, value):
        if attr in self.__slots__:
            return super(DBusIndirectObject, self).__setattr__(attr, value)
        return setattr(self.real_object, attr, value)

    def get_dbus_method(self, *args, **kwargs):
        def wrap(*m_args, **m_kwargs):
            def invoke():
                m = self.real_object.get_dbus_method(*args, **kwargs)
                return m(*m_args, **m_kwargs)
            try:
                return invoke()
            except dbus.exceptions.DBusException, exception:
                try:
                    dbus_name = str (exception.get_dbus_name ())
                except AttributeError:
                    dbus_name = ""
                if dbus_name == "org.freedesktop.DBus.Error.ServiceUnknown":
                    self.bind_object()
                    return invoke()
                else:
                    raise

        return wrap

class _BaseObject(object):
    """
    _Object, _Stream and _Manager inherit from this class, which
    implements common functionality, such as getting and setting
    properties.
    """
    @_check_main_thread
    def __init__(self, initial_properties, property_map):
        super (_BaseObject, self).__init__ ()

        # Check the properties.
        for k in initial_properties.keys ():
            assert k in property_map

        # self.properties is a dictionary mapping property names to
        # values and the time that they were looked up.
        now = time.time ()
        self.properties \
            = dict ([[k, [ v, now ]] for k, v in initial_properties.items ()])
        self.property_map = property_map

        try:
            self.dbus_properties = dbus.Interface(
                self.proxy,
                dbus_interface='org.freedesktop.DBus.Properties')
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

    @_check_main_thread
    def __getattribute__(self, name):
        try:
            property_map = (super (_BaseObject, self)
                            .__getattribute__('property_map'))
        except AttributeError:
            property_map = {}

        if name not in property_map:
            return super(_BaseObject, self).__getattribute__(name)

        properties = self.__dict__['properties']

        if (name in properties
            and (time.time () - properties[name][1] < property_map[name][3])):
            # name is cached and it is still valid.
            return properties[name][0]

        try:
            value = self.dbus_properties.Get(
                dbus.String(""), dbus.String(property_map[name][0]))
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

        if property_map[name][3] is not None:
            # TTL is not None.  Cache the value.
            properties[name] = [ value, time.time () ]

        return value

    @_check_main_thread
    def __setattr__(self, name, value):
        if ('property_map' in self.__dict__ and name in self.property_map):
            # Write through...
            try:
                self.dbus_properties.Set(
                    dbus.String(""), dbus.String(self.property_map[name][0]),
                    self.property_map[name][1](value))
            except dbus.exceptions.DBusException, exception:
                _dbus_exception_to_woodchuck_exception(exception)
            if self.property_map[name][3] is not None:
                # Cache the value.
                self.properties[name] = [ value, time.time () ]

        super(_BaseObject, self).__setattr__(name, value)

    def __repr__(self):
        return ("woodchuck." + self.__class__.__name__ + "("
                + dict([[k, v[0]] for k, v in self.properties.items ()
                        if k in [ 'UUID', 'parent_UUID', 'human_readable_name' ]
                        ]).__repr__()
                + ")")
    def __str__(self):
        return self.__repr__ ().__str__ ()

class _Object(_BaseObject):
    """The local representation for a Woodchuck object."""
    @_check_main_thread
    def __init__(self, **properties):
        """
        Instantiate a :class:`Woodchuck._Object`.  Instantiating this
        object does not actually register an object; the object is
        assumed to already exist.  A :class:`Woodchuck._Object` object
        should should not normally be directly instantiated from user
        code.  Instead, use a method that returns an :class:`_Object`,
        such as :func:`_Stream.object_register` or
        :func:`_Stream.lookup_object_by_cookie` to get a
        :class:`_Object` object.

        :param UUID: The UUID of the object.

        :param properties: Other properties, e.g.,
            human_readable_name.  Assumed to correspond to stream's
            actual values.
        """

        self.proxy = DBusIndirectObject(
            dbus.SessionBus(),
            'org.woodchuck', '/org/woodchuck/object/' + properties['UUID'],
            introspect=False)
        try:
            self.dbus = dbus.Interface (self.proxy,
                                        dbus_interface='org.woodchuck.object')
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception (exception)

        super(_Object, self).__init__ (properties,
                                       _object_properties_to_camel_case)

    @_check_main_thread
    def unregister(self):
        """Unregister the object object thereby causing Woodchuck to
        permanently forget about the object.

        See :func:`_Stream.unregister` for an example using a similar
        function.
        """
        try:
            ret = self.dbus.Unregister ()
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception (exception)

        if ret:
            del _objects[self.UUID]

    @_check_main_thread
    def transfer(self, request_type):
        """
        Request that Woodchuck transfer the object.  This only makes
        sense for object's that use Woodchuck's simple transferer.

        :param request_type: Whether the request is user initiated or
            application initiated.  See :class:`TransferStatus` for
            possible values.
        """
        try:
            self.dbus.Transfer(dbus.UInt32(request_type))
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception (exception)

    @_check_main_thread
    def transfer_status(self, status, indicator=None,
                        transferred_up=None, transferred_down=None,
                        transfer_time=None, transfer_duration=None,
                        object_size=None, files=None):
        """
        Tell Woodchuck that the object has been transferred.  Call
        this function whenever an object is transferred (or uploaded),
        not only in response to a :func:`_Upcalls.object_transfer_cb`
        upcall.

        :param status: On success, 0.  Otherwise, the error code.  See
            :class:`TransferStatus` for possible values.

        :param indicator: What indicators, if any, were shown to the
            user indicating that the stream was updated.  A bit-wise
            mask of :class:`Indicator`.  Default: None.

        :param transferred_up: The number of bytes uploaded.  If not
            known, set to None.  Default: None.

        :param transferred_down: The number of bytes transferred.  If
            not known, set to None.  Default: None.

        :param transfer_time: The time at which the update was
            started.  If not known, set to None.  Default: None.

        :param transfer_duration: The amount of time the update took,
            in seconds.  If not known, set to None.  Default: None.

        :param object_size: The amount of disk space used by the
            object, in bytes.  If not known, set to None.  Default:
            None.

        :param files: The files belong to the object.  An array of
            arrays consisting of a filename (a string), a boolean
            indicating whether the file is exclusive to the object,
            and the file's deletion policy (see
            :class:`woodchuck.DeletionPolicy` for possible values).

        Example of reporting an object transfer for an object that
        Woodchuck can deleted without consulting the user::

          transfer_time = int (time.time ())
          ...
          # Perform the transfer
          ...
          transfer_duration = int (time.time ()) - transfer_time
          stream.update_status(
              status=0,
              transferred_up=4096,
              transferred_down=1024000,
              transfer_time=transfer_time,
              transfer_duration=transfer_duration,
              files=( ("/home/user/Podcasts/Foo/Episode1.ogg", True,
                       woodchuck.DeletionPolicy.DeleteWithoutConsultation),))
        """
        status = dbus.UInt32(status)

        if indicator is None:
            indicator = 0x80000000
        indicator = dbus.UInt32(indicator)

        if transferred_up is None:
            transferred_up = 2 ** 64 - 1
        transferred_up = dbus.UInt64(transferred_up)

        if transferred_down is None:
            transferred_down = 2 ** 64 - 1
        transferred_down = dbus.UInt64(transferred_down)

        if transfer_time is None:
            transfer_time = int (time.time ())
        transfer_time = dbus.UInt64(transfer_time)

        if transfer_duration is None:
            transfer_duration = 0
        transfer_duration = dbus.UInt32(transfer_duration)

        if object_size is None:
            object_size = 2 ** 64 - 1
        object_size = dbus.UInt64(object_size)

        files = dbus.Array(files if files is not None else [], "(sbu)")

        try:
            self.dbus.TransferStatus(status, indicator,
                                     transferred_up, transferred_down,
                                     transfer_time, transfer_duration,
                                     object_size, files)
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

    @_check_main_thread
    def used(self, start=None, duration=None, use_mask=None):
        """
        Mark the object as having been used.

        :param start: The time at which the user started using the
            object.  If unknown, pass None.  Default: None.

        :param duration: The amount of time the user used the object,
            in seconds.  If unknown, pass None.  Default: None.

        :param use_mask: A 64-bit mask indicating the parts of the
            object that were used.  Setting the least-significant bit
            means that the first 1/64 of the object was used, the
            second bit means that the second 1/64 of the object was
            used, etc.  If unknown, pass None.  Default: None.

        Example: Indicate that that the first two minutes of an
        hour-long video were viewed::

            object.used(start_time, 120, 0x3)
        """
        if start is None:
            start = int (time.time ())
        start = dbus.UInt64(start)
        if duration is None:
            duration = 2 ** 64 - 1
        duration = dbus.UInt64(duration)
        if use_mask is None:
            use_mask = 2 ** 64 - 1
        use_mask = dbus.UInt64(use_mask)

        try:
            self.dbus.Used(start, duration, use_mask)
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception (exception)

    @_check_main_thread
    def files_deleted(self, update=None, arg=None):
        """
        Indicate that some or all of the object's files have been
        deleted.  This should be called whenever an object's files are
        deleted, not only in response to
        :func:`Upcalls.object_delete_files_cb`.

        :param update: Taken from :class:`woodchuck.DeletionResponse`.

        :param arg: If update is DeletionResponse.Deleted, the value
            is ignored.

            If update is DeletionResponse.Refused, the value is the
            minimum number of seconds the object should be preserved,
            i.e., the minimum amount of time before Woodchuck should
            call :func:`Upcalls.object_delete_files_cb` again.

            If update is DeletionResponse.Compressed, the value is the
            number of bytes of disk space the object now uses.

        Example: An email's attachments are purged, but the body is
        preserved::

            object.files_deleted (woodchuck.DeletionResponse.Compressed,
                                  2338)
        """
        if update is None or update == DeletionResponse.Deleted:
            update = DeletionResponse.Deleted
            arg = 0
        elif arg is None:
            if update == DeletionResponse.Refused:
                arg = int (time.time ()) + 5 * 60
            elif update == DeletionResponse.Compressed:
                arg = 2 ** 64 - 1

        update = dbus.UInt32(update)
        arg = dbus.UInt64(arg)
        
        try:
            self.dbus.FilesDeleted(update, arg)
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception (exception)

_objects = {}
def Object(**properties):
    """Return a reference to a :class:`_Object` object.  This function
    does not actually register the object; the manager is assumed to
    already exist.  This function should not normally be called from
    user code.  Instead, call :func:`_Stream.object_register` or
    :func:`_Stream.lookup_object_by_cookie` to get a :class:`_Object`
    object.

    :param UUID: The object's UUID, required.

    :param properties: Other properties, e.g., human_readable_name.
        Assumed to correspond to the manager's actual values.

    :returns: A :class:`_Object` object with the specified
        properties.

    Note: There is at most a single :class:`_Object` instance per
    Woodchuck object.  In other words, the Python object is shared
    among all users."""
    if properties['UUID'] in _objects:
        return _objects[properties['UUID']]
    return _Object(**properties)

class _Stream(_BaseObject):
    @_check_main_thread
    def __init__(self, **properties):
        """Instantiate a :class:`Woodchuck._Stream`.  Instantiating
        this object does not actually register a stream; the stream is
        assumed to already exist.  A :class:`Woodchuck._Stream` object
        should should not normally be directly instantiated from user
        code.  Instead, use a method that returns an :class:`_Stream`,
        such as :func:`_Manager.stream_register` or
        :func:`_Manager.lookup_stream_by_cookie` to get a
        :class:`_Stream` object.

        :param UUID: The UUID of the stream.

        :param properties: Other properties, e.g.,
            human_readable_name.  Assumed to correspond to stream's
            actual values.
        """

        self.proxy = DBusIndirectObject(
            dbus.SessionBus(),
            'org.woodchuck', '/org/woodchuck/stream/' + properties['UUID'],
            introspect=False)
        try:
            self.dbus = dbus.Interface (self.proxy,
                                        dbus_interface='org.woodchuck.stream')
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception (exception)

        super(_Stream, self).__init__ (properties,
                                       _stream_properties_to_camel_case)

    @_check_main_thread
    def unregister(self, only_if_empty):
        """Unregister the stream object thereby causing Woodchuck to
        permanently forget about the stream and any object it
        contained.

        :param only_if_empty: If True, this method invocation only
            suceeds if the stream contains no objects.

        Example::

          try:
              stream.unregister (True)
          except woodchuck.NoSuchObject as exception:
              print "Can't remove stream %s: Does not exist: %s"
                  % (str (stream), exception)
          except woodchuck.ObjectExistsError as exception:
              print "Can't remove stream %s: Not empty: %s"
                  % (str (stream), exception)
        """
        try:
            ret = self.dbus.Unregister(dbus.Boolean(only_if_empty))
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

        if ret:
            del _streams[self.UUID]

    @_check_main_thread
    def object_register(self, only_if_cookie_unique=True, **properties):
        """Register a new object.
    
        :param only_if_cookie_unique: If True, only succeed if the
            specified cookie is unique.
    
        :param human_readable_name: A string that can be shown to the
            user that identifies the object.

        :param properties: Other properties to set.
    
        :returns: A :class:`_Object` object.

        See :func:`_Manager.stream_register` for an example using a
        similar function.
        """
        try:
            UUID = self.dbus.ObjectRegister \
                (dbus.Dictionary(
                    _keys_convert(properties,
                                  _object_properties_to_camel_case),
                    'sv'),
                 dbus.Boolean(only_if_cookie_unique))
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

        properties['UUID'] = UUID
        return Object(**properties)

    @_check_main_thread
    def list_objects(self):
        """List this stream's objects.

        :returns: An array of :class:`_Object`

        See :func:`_Woodchuck.list_managers` for an example using a
        similar function.
        """
        try:
            return [Object(UUID=UUID, human_readable_name=human_readable_name,
                           cookie=cookie, parent_UUID=self.UUID)
                    for UUID, cookie, human_readable_name
                    in self.dbus.ListObjects()]
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

    @_check_main_thread
    def lookup_object_by_cookie(self, cookie):
        """Return the set of objects with the specified cookie.

        :param cookie: The cookie to match.

        :returns: An array of :class:`_Object`

        See :func:`_Woodchuck.lookup_manager_by_cookie` for an example
        using a similar function.
        """
        try:
            return [Object(UUID=UUID, human_readable_name=human_readable_name,
                           cookie=cookie, parent_UUID=self.UUID)
                    for UUID, human_readable_name
                    in self.dbus.LookupObjectByCookie(
                        dbus.String(_str_to_dbus_str(cookie)))]
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

    @_check_main_thread
    def update_status(self, status, indicator=None,
                      transferred_up=None, transferred_down=None,
                      transfer_time=None, transfer_duration=None,
                      new_objects=None, updated_objects=None,
                      objects_inline=None):
        """
        Tell Woodchuck that the stream has been updated.  Call this
        function whenever a stream is updated, not only in response to
        a :func:`_Upcalls.stream_update_cb` upcall.

        :param status: On success, 0.  Otherwise, the error code.  See
            :class:`TransferStatus` for possible values.

        :param indicator: What indicators, if any, were shown to the
            user indicating that the stream was updated.  A bit-wise
            mask of :class:`Indicator`.  Default: None.

        :param transferred_up: The number of bytes uploaded.  If not
            known, set to None.  Default: None.

        :param transferred_down: The number of bytes transferred.  If
            not known, set to None.  Default: None.

        :param transfer_time: The time at which the update was
            started.  If not known, set to None.  Default: None.

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

            import woodchuck
            import time

            ...
            
            transfer_time = int (time.time ())
            ...
            # Perform the transfer
            ...
            transfer_duration = int (time.time ()) - transfer_time
            stream.update_status (status=0,
                                  transferred_up=2048,
                                  transferred_down=64000,
                                  transfer_time=transfer_time,
                                  transfer_duration=transfer_duration,
                                  new_objects=5,
                                  objects_inline=5)

        Note: The five new objects should immediately be registered
        using :func:`_Stream.object_register` and marked as transferred
        using :func:`_Object.transfer_status`.

        Example of a failed update due to a network problem, e.g., the
        host is unreachable::

          stream.update_status (woodchuck.TransientNetwork,
                                transferred_up=100)
        """
        status = dbus.UInt32(status)

        if indicator is None:
            indicator = 0
        indicator = dbus.UInt32(indicator)

        if transferred_up is None:
            transferred_up = 2 ** 64 - 1
        transferred_up = dbus.UInt64(transferred_up)

        if transferred_down is None:
            transferred_down = 2 ** 64 - 1
        transferred_down = dbus.UInt64(transferred_down)

        if transfer_time is None:
            transfer_time = 0
        transfer_time = dbus.UInt64(transfer_time)

        if transfer_duration is None:
            transfer_duration = 0
        transfer_duration = dbus.UInt32(transfer_duration)

        if new_objects is None:
            new_objects = 2 ** 32 - 1
        new_objects = dbus.UInt32(new_objects)

        if updated_objects is None:
            updated_objects = 2 ** 32 - 1
        updated_objects = dbus.UInt32(updated_objects)

        if objects_inline is None:
            objects_inline = 2 ** 32 - 1
        objects_inline = dbus.UInt32(objects_inline)

        try:
            self.dbus.UpdateStatus(status, indicator,
                                   transferred_up, transferred_down,
                                   transfer_time, transfer_duration,
                                   new_objects, updated_objects, objects_inline)
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

_streams = {}
def Stream(**properties):
    """Return a reference to a :class:`_Stream` object.  This function
    does not actually register a stream; a stream is assumed to
    already exist.  This function should not normally be called from
    user code.  Instead, call :func:`_Manager.stream_register` or
    :func:`_Manager.lookup_stream_by_cookie` to get a :class:`_Stream`
    object.

    :param UUID: The stream's UUID, required.

    :param properties: Other properties, e.g., human_readable_name.
        Assumed to correspond to the manager's actual values.

    :returns: A :class:`_Stream` object with the specified
        properties.

    Note: There is at most a single :class:`_Stream` instance per
    Woodchuck stream object.  In other words, the Python object is
    shared among all users."""
    if properties['UUID'] in _streams:
        return _streams[properties['UUID']]
    return _Stream(**properties)

class _Manager(_BaseObject):
    @_check_main_thread
    def __init__(self, **properties):
        """Instantiate a :class:`Woodchuck._Manager`.  Instantiating
        this object does not actually register a manager; the manager
        is assumed to already exist.  A :class:`Woodchuck._Manager`
        object should should not normally be directly instantiated
        from user code.  Instead, use a method that returns an
        :class:`_Manager`, such as :func:`_Woodchuck.manager_register`
        or :func:`_Woodchuck.lookup_manager_by_cookie` to get a
        :class:`_Manager` object.

        :param UUID: The UUID of the Manager.

        :param properties: Other properties, e.g.,
            human_readable_name.  Assumed to correspond to the
            manager's actual values.
        """

        self.proxy = DBusIndirectObject(
            dbus.SessionBus(),
            'org.woodchuck', '/org/woodchuck/manager/' + properties['UUID'],
            introspect=False)
        try:
            self.dbus = dbus.Interface (self.proxy,
                                        dbus_interface='org.woodchuck.manager')
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception (exception)

        # [0]: descendents_too = False; [1]: descendents_too = True
        self.feedback_subscriptions = [ 0, 0 ];
        self.feedback_subscription_handle = None;

        super(_Manager, self).__init__ (properties,
                                        _manager_properties_to_camel_case)

    @_check_main_thread
    def unregister(self, only_if_empty=True):
        """Unregister the manager object thereby causing Woodchuck to
        permanently forget about the manager and any streams and
        objects it contained.

        :param only_if_empty: If True, this method invocation only
            suceeds if the manager has no children, i.e., no
            descendent managers and no streams.

        Example::

          try:
              manager.unregister (True)
          except woodchuck.NoSuchObject as exception:
              print "Can't remove stream %s: Does not exist: %s" \\
                  % (str (manager), exception)
          except woodchuck.ObjectExistsError as exception:
              print "Can't remove manager %s: Not empty: %s" \\
                  % (str (manager), exception)"""
        try:
            ret = self.dbus.Unregister(dbus.Boolean(only_if_empty))
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)
        if ret:
            del _managers[self.UUID]

    @_check_main_thread
    def manager_register(self, only_if_cookie_unique=True,
                         **properties):
        """Register a child manager.

        :param only_if_cookie_unique: If True, only succeed if the
            specified cookie is unique among sibling managers.
    
        :param human_readable_name: A string that can be shown to the
            user that identifies the manager.

        :param properties: Other properties to set.
    
        :returns: A :class:`_Manager` object.

        Example::
    
            import woodchuck

            w = woodchuck.Woodchuck ()
            manager = w.manager_register(
                only_if_cookie_unique=True,
                human_readable_name="Web Browser",
                cookie="org.webbrowser",
                dbus_service_name="org.webbrowser")

            web_cache = manager.manager_register(
                only_if_cookie_unique=False,
                human_readable_name="Web Cache")

            download_later = manager.manager_register(
                only_if_cookie_unique=False,
                human_readable_name="Downloads for Later")

            manager.unregister (only_if_empty=False)
        """
        try:
            UUID = self.dbus.ManagerRegister \
                (dbus.Dictionary(
                    _keys_convert(properties,
                                  _manager_properties_to_camel_case),
                    'sv'),
                 dbus.Boolean(only_if_cookie_unique))
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

        properties['UUID'] = UUID
        return Manager(**properties)

    @_check_main_thread
    def list_managers(self, recursive=False):
        """List managers that are a descendent of this one.

        :param recursive: If True, list all descendent managers.
            Otherwise, only list managers that are an immediate
            descendent.

        :returns: An array of :class:`_Manager`

        See :func:`_Woodchuck.list_managers` for an example using a
        similar function.
        """
        try:
            return [Manager(UUID=UUID, human_readable_name=human_readable_name,
                            cookie=cookie, parent_UUID=parent_UUID)
                    for UUID, cookie, human_readable_name, parent_UUID
                    in self.dbus.ListManagers(dbus.Boolean(recursive))]
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

    @_check_main_thread
    def lookup_manager_by_cookie(self, cookie, recursive=False):
        """Return the set of managers with the specified cookie that
        are a descendent of this one.

        :param cookie: The cookie to lookup.

        :param recursive: If False, only consider immediate children,
            otherwise, consider any descendent.

        :returns: An array of :class:`_Manager`

        See :func:`_Woodchuck.lookup_manager_by_cookie` for an example.
        """
        try:
            return [Manager(UUID=UUID, human_readable_name=human_readable_name,
                            cookie=cookie, parent_UUID=parent_UUID)
                    for UUID, human_readable_name, parent_UUID
                    in self.dbus.LookupManagerByCookie(
                        dbus.String(_str_to_dbus_str(cookie)),
                        dbus.Boolean(recursive))]
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception (exception)

    @_check_main_thread
    def stream_register(self, only_if_cookie_unique=True, **properties):
        """Register a new stream.
    
        :param only_if_cookie_unique: If True, only succeed if the
            specified cookie is unique.
    
        :param human_readable_name: A string that can be shown to the
            user that identifies the stream.

        :param properties: Other properties to set.
    
        :returns: A :class:`_Stream` object.

        Example::
    
            import woodchuck
            import random

            w = woodchuck.Woodchuck()

            cookie=str (random.random())
            m = w.manager_register(True, cookie=cookie,
                human_readable_name="Test Manager")

            s = m.stream_register(True, cookie=cookie,
                human_readable_name="Test Stream")

            print m.list_streams ()

            m.unregister (only_if_empty=False)
        """
        try:
            UUID = self.dbus.StreamRegister \
                (dbus.Dictionary(
                    _keys_convert(properties,
                                  _stream_properties_to_camel_case),
                    'sv'),
                 dbus.Boolean(only_if_cookie_unique))
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

        properties['UUID'] = UUID
        return Stream(**properties)

    @_check_main_thread
    def list_streams(self):
        """List this manager's streams.

        :returns: An array of :class:`_Stream`

        See :func:`_Woodchuck.list_managers` for an example using a
        similar function.
        """
        try:
            return [Stream(UUID=UUID, human_readable_name=human_readable_name,
                           cookie=cookie, parent_UUID=self.UUID)
                    for UUID, cookie, human_readable_name
                    in self.dbus.ListStreams()]
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

    @_check_main_thread
    def lookup_stream_by_cookie(self, cookie):
        """Return the set of streams with the specified cookie.

        :param cookie: The cookie to match.

        :returns: An array of :class:`_Stream`

        See :func:`_Woodchuck.lookup_manager_by_cookie` for an example
        using a similar function.
        """
        try:
            return [Stream(UUID=UUID, human_readable_name=human_readable_name,
                           cookie=cookie, parent_UUID=self.UUID)
                    for UUID, human_readable_name
                    in self.dbus.LookupStreamByCookie(
                        dbus.String(_str_to_dbus_str(cookie)))]
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception (exception)

    @_check_main_thread
    def feedback_subscribe(self, descendents_too=True):
        """Request that Woodchuck begin making upcalls for this
        manager.

        :param descendents_too: If True, also makes upcalls for any
              descendent managers.

        :returns: An opaque handle, which must be passed to
              :func:`_Manager.feedback_unsubscribe`.

        At most, a single subscription is obtained per Manager.  Thus,
        multiple subscriptions share the same handle.  To stop
        receiving feedback, :func:`_Manager.feedback_unsubscribe` must
        be called the same number of times.

        Example::

            subscription = manager.feedback_subscribe (True)
            ...
            manager.feedback_unsubscribe(subscription)

        To actually receive upcalls refer to
        :class:`woodchuck.Upcalls`."""
        if descendents_too:
            idx = 1
        else:
            idx = 0

        if self.feedback_subscription_handle:
            # We already have a subscription.
            if descendents_too and self.feedback_subscriptions[1] == 0:
                # It does not include descendents, but the new
                # subscriber wants descendents.  Get a new
                # subscription (which can be shared).
                assert self.feedback_subscriptions[0] > 0

                h = self.dbus.FeedbackSubscribe(dbus.Boolean(True))
                self.dbus.FeedbackUnsubscribe(
                    dbus.String(self.feedback_subscription_handle))
                self.feedback_subscription_handle = h

                self.feedback_subscriptions[idx] += 1
            else:
                # We have an acceptable subscription.
                self.feedback_subscriptions[idx] += 1
        else:
            # No subscriptions yet.
            assert self.feedback_subscriptions[0] == 0
            assert self.feedback_subscriptions[1] == 0

            self.feedback_subscription_handle \
                = self.dbus.FeedbackSubscribe(dbus.Boolean(descendents_too))
            self.feedback_subscriptions[idx] = 1

        return idx

    @_check_main_thread
    def feedback_unsubscribe(self, handle):
        """Cancel an upcall subscription.

        :param handle: The value returned by a previous call to
            :func:`_Manager.feedback_subscribe`.
        """

        assert handle == 0 or handle == 1
        assert self.feedback_subscriptions[handle] > 0

        if (self.feedback_subscriptions[0]
            + self.feedback_subscriptions[1] == 1):
            # No subscriptions left.
            self.dbus.FeedbackUnsubscribe(
                dbus.String(self.feedback_subscription_handle))
            self.feedback_subscription_handle = None

            self.feedback_subscriptions[handle] -= 1
            return

        if (handle == 1
            and self.feedback_subscriptions[1] == 1
            and self.feedback_subscriptions[0] > 0):
            # The last descendents_too subscription is now gone, but
            # we still have non-descendent subscriptions.  Get an
            # appropriate subscription.
            h = self.dbus.FeedbackSubscribe(dbus.Boolean(False))
            self.dbus.FeedbackUnsubscribe(
                dbus.String(self.feedback_subscription_handle))
            self.feedback_subscription_handle = h

            self.feedback_subscriptions[1] = 0
            return

    @_check_main_thread
    def feedback_ack(self, object_UUID, object_instance):
        """Invoke org.woodchuck.manager.FeedbackAck."""
        try:
            self.dbus.FeedbackAck(
                dbus.String(_str_to_dbus_str(object_UUID)),
                dbus.UInt32(object_instance))
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

_managers = {}
def Manager(**properties):
    """Return a reference to a :class:`_Manager` object.  This
    function does not actually register a manager; a manager is
    assumed to already exist.  This function should not normally be
    called from user code.  Instead, call
    :func:`_Woodchuck.manager_register` or
    :func:`_Woodchuck.lookup_manager_by_cookie` to get a
    :class:`_Manager` object.

    :param UUID: The manager's UUID, required.

    :param properties: Other properties, e.g., human_readable_name.
        Assumed to correspond to the manager's actual values.

    :returns: A :class:`_Manager` object with the specified
        properties.

    Note: There is at most a single :class:`_Manager` instance per
    Woodchuck manager object.  In other words, the Python object is
    shared among all users."""
    if properties['UUID'] in _managers:
        return _managers[properties['UUID']]
    return _Manager(**properties)

class _Woodchuck(object):
    """The top-level Woodchuck class."""

    @_check_main_thread
    def __init__(self):
        # Establish a connection with the Woodchuck server.
        try:
            self._woodchuck_object = DBusIndirectObject(
                dbus.SessionBus(),
                'org.woodchuck', '/org/woodchuck', introspect=False)
            self._woodchuck \
                = dbus.Interface (self._woodchuck_object,
                                  dbus_interface='org.woodchuck')
        except dbus.exceptions.DBusException, exception:
            self._woodchuck_object = None
            self._woodchuck = None
            _dbus_exception_to_woodchuck_exception (exception)

    @_check_main_thread
    def manager_register(self, only_if_cookie_unique=True, **properties):
        """Register a new top-level manager.
    
        :param only_if_cookie_unique: If True, only succeed if the
            specified cookie is unique among top-level managers.
    
        :param human_readable_name: A string that can be shown to the
            user that identifies the manager.

        :param properties: Other properties to set.
    
        :returns: A :class:`_Manager` object.

        Example::
    
            import woodchuck

            w = woodchuck.Woodchuck ()
            manager = w.manager_register(
                only_if_cookie_unique=True,
                human_readable_name="RSS Reader",
                cookie="org.rssreader",
                dbus_service_name="org.rssreader")
            manager.unregister ()
        """
        assert 'parent_UUID' not in properties
        try:
            UUID = self._woodchuck.ManagerRegister \
                (dbus.Dictionary(
                    _keys_convert(properties,
                                  _manager_properties_to_camel_case)),
                 dbus.Boolean(only_if_cookie_unique))
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)
    
        properties['UUID'] = UUID
        return Manager(**properties)
    
    @_check_main_thread
    def list_managers(self, recursive=False):
        """List known managers.

        :param recursive: If True, list all managers.  Otherwise, only
            list top-level managers.

        :returns: An array of :class:`_Manager`

        Example::

            import woodchuck
            print "The top-level managers are:"
            for m in woodchuck.Woodchuck().list_managers (False):
                print m.human_readable_name + ": " + m.cookie
        """
        try:
            return [Manager(UUID=UUID, human_readable_name=human_readable_name,
                            cookie=cookie, parent_UUID=parent_UUID)
                    for UUID, cookie, human_readable_name, parent_UUID
                    in self._woodchuck.ListManagers(dbus.Boolean(recursive))]
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)
    
    @_check_main_thread
    def lookup_manager_by_cookie(self, cookie, recursive=False):
        """Return the set of managers with the specified cookie.

        :param cookie: The cookie to lookup.

        :param recursive: If False, only consider top-level managers,
            otherwise, consider any manager.

        :returns: An array of :class:`_Manager`

        Example::

          import woodchuck
          import random

          w = woodchuck.Woodchuck()

          cookie=str (random.random())
          m = w.manager_register(True, cookie=cookie,
              human_readable_name="Test")

          managers = w.lookup_manager_by_cookie(cookie, False)
          assert len (managers) == 1
          assert managers[0].UUID == m.UUID
          assert managers[0].cookie == cookie
          m.unregister (True)
          """
        try:
            return [Manager(UUID=UUID, human_readable_name=human_readable_name,
                            cookie=cookie, parent_UUID=parent_UUID)
                    for UUID, human_readable_name, parent_UUID
                    in self._woodchuck.LookupManagerByCookie(
                        dbus.String(_str_to_dbus_str(cookie)),
                        dbus.Boolean(recursive))]
        except dbus.exceptions.DBusException, exception:
            _dbus_exception_to_woodchuck_exception(exception)

_woodchuck = None
def Woodchuck():
    """Return a reference to the top-level Woodchuck singleton.

    Note: There is at most a single :class:`_Woodchuck` instance.  In
    other words, the Python object is shared among all users."""
    global _woodchuck
    if _woodchuck is None:
        _woodchuck = _Woodchuck()
    return _woodchuck

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
        logger.debug("Message from %s, expected message from %s"
                     % (name, _woodchuck_owner))

    return name == _woodchuck_owner

class Upcalls(dbus.service.Object):
    """A thin wrapper around org.woodchuck.upcalls.

    To use this class, implement your own class, which inherits from
    this one and overrides the virtual methods of the upcalls that you
    are interested in (:func:`Upcalls.object_transferred_cb`,
    :func:`Upcalls.stream_update_cb`,
    :func:`Upcalls.object_transfer_cb` and
    :func:`object_delete_files_cb`).  Instantiate the class and then
    call :func:`woodchuck.feedback_subscribe` to begin receiving
    feedback.

    Example::

        class Upcalls(woodchuck.Upcalls):
            def object_transferred_cb (self, **kwargs):
                # Transfer the kwargs[object_UUID] object.
                ...
        upcalls = Upcalls ()
        subscription = Manager.feedback_subscribe (False)

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
    @_check_main_thread
    def __init__(self, path):
        """
        :param path: The object that will receive the upcalls from
            woodchuck.
        """
        bus = dbus.SessionBus()

        try:
            dbus.service.Object.__init__(self, bus, path)
        except RuntimeError, e:
            s = """
A RunTime error occured while trying to connect to DBus.  Did you
forget to include

    from dbus.mainloop.glib import DBusGMainLoop
    DBusGMainLoop(set_as_default=True)

or

    from dbus.mainloop.qt import DBusQtMainLoop
    DBusQtMainLoop(set_as_default=True)

before calling any DBus functions (or anything that uses DBus, such as
Woodchuck)?"""
            logger.error(s)
            print >> sys.stderr, s
            raise

        if not _watching_woodchuck_owner:
            bus.watch_name_owner ("org.woodchuck", _woodchuck_owner_update)
            try:
                owner = bus.get_name_owner ("org.woodchuck")
            except dbus.exceptions.DBusException, exception:
                if (exception.get_dbus_name () ==
                    "org.freedesktop.DBus.Error.NameHasNoOwner"):
                    raise WoodchuckUnavailableError (
                        exception.get_dbus_message ())
                else:
                    raise

            _woodchuck_owner_update (owner)

    @dbus.service.method(dbus_interface='org.woodchuck.upcall',
                         in_signature='ssssssuu(ustub)sttt',
                         out_signature='',
                         sender_keyword="sender")
    def ObjectTransferred(self, manager_UUID, manager_cookie,
                         stream_UUID, stream_cookie,
                         object_UUID, object_cookie,
                         status, instance, version, filename, size,
                         trigger_target, trigger_fired, sender):
        if not _is_woodchuck (sender):
            return False

        return self.object_transferred_cb(manager_UUID, manager_cookie,
                                          stream_UUID, stream_cookie,
                                          object_UUID, object_cookie,
                                          status, instance, version,
                                          filename, size,
                                          trigger_target, trigger_fired)

    def object_transferred_cb(self, manager_UUID, manager_cookie,
                              stream_UUID, stream_cookie,
                              object_UUID, object_cookie,
                              status, instance, version, filename, size,
                              trigger_target, trigger_fired):
        """Virtual method that should be implemented by the child
        class if it is interested in
        org.woodchuck.upcall.ObjectTransferred upcalls.

        This upcall is invoked when Woodchuck transfers an object on
        behalf of a manager.  This is only done for objects using the
        simple transferer.

        :param manager_UUID: The manager's UUID.

        :param manager_cookie: The manager's cookie.

        :param stream_UUID: The stream's UUID.

        :param stream_cookie: The stream's cookie.

        :param object_UUID: The object's UUID.

        :param object_cookie: The object's cookie.

        :param status: Whether the transfer was successfully.  The
            value is taken from :class:`woodchuck.TransferStatus`.

        :param instance: The number of transfer attempts (not
            including this one).

        :param version: The version that was transferred.  An array of:
            the index in the version array, the URL, the expected
            object size on disk (negative if transferring the object
            will free space), the expected upload size, the expected
            transfer size, the utility and the value of use simple
            transferer.

        :param filename: The name of the file containing the data.

        :param size: The size of the file, in bytes.

        :param trigger_target: The time the application requested the
            object be transferred.

        :param trigger_fired: The time at which the file was actually
            transferred.
        """
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
        org.woodchuck.upcall.StreamUpdate upcalls.

        This upcall is invoked when a stream should be updated.  The
        application should update the stream and call
        :func:`_Stream.update_status`.

        :param manager_UUID: The manager's UUID.

        :param manager_cookie: The manager's cookie.

        :param stream_UUID: The stream's UUID.

        :param stream_cookie: The stream's cookie.
        """
        pass
    
    @dbus.service.method(dbus_interface='org.woodchuck.upcall',
                         in_signature='ssssss(usxttub)su', out_signature='',
                         sender_keyword="sender")
    def ObjectTransfer(self, manager_UUID, manager_cookie,
                       stream_UUID, stream_cookie,
                       object_UUID, object_cookie,
                       version, filename, quality, sender):
        if not _is_woodchuck (sender):
            return False

        self.object_transfer_cb (manager_UUID, manager_cookie,
                                 stream_UUID, stream_cookie,
                                 object_UUID, object_cookie,
                                 version, filename, quality)

    def object_transfer_cb (self, manager_UUID, manager_cookie,
                            stream_UUID, stream_cookie,
                            object_UUID, object_cookie,
                            version, filename, quality):
        """Virtual method that should be implemented by the child
        class if it is interested in
        org.woodchuck.upcall.ObjectTransfer upcalls.

        This upcall is invoked when Woodchuck transfers an object on
        behalf of a manager.  This is only done for objects using the
        simple transferer.

        :param manager_UUID: The manager's UUID.

        :param manager_cookie: The manager's cookie.

        :param stream_UUID: The stream's UUID.

        :param stream_cookie: The stream's cookie.

        :param object_UUID: The object's UUID.

        :param object_cookie: The object's cookie.

        :param version: The version to transfer.  the index in the
            version array, the URL, the expected object size on disk
            (negative if transferring the object will free space), the
            expected upload size, the expected transfer size, the
            utility and the value of use simple transferer.

        :param filename: The name of the filename property.

        :param quality: The degree to which quality should be
            sacrified to reduce the number of bytes transferred.  The
            target quality of the transfer.  From 1 (most compressed)
            to 5 (highest available fidelity).
        """
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
        org.woodchuck.upcall.ObjectDeleteFiles upcalls.

        This upcall is invoked when Woodchuck wants a manager to free
        disk space.

        :param manager_UUID: The manager's UUID.

        :param manager_cookie: The manager's cookie.

        :param stream_UUID: The stream's UUID.

        :param stream_cookie: The stream's cookie.

        :param object_UUID: The object's UUID.

        :param object_cookie: The object's cookie.
        """
        pass

if __name__ == "__main__":
    import random
    cookie = str (random.random ())
    print Woodchuck().list_managers()
    manager = Woodchuck().manager_register \
        (human_readable_name="Test", cookie=cookie,
         only_if_cookie_unique=True)
    try:
        have_one = 0
        for m in Woodchuck().lookup_manager_by_cookie (cookie, False):
            print "Considering: " + str (m)
            if m.properties['cookie'] == cookie:
                have_one += 1
        if have_one == 0:
            print "Failed to find manager we just registered!"
    finally:
        manager.unregister (True)
