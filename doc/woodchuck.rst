Woodchuck
=========

Contents:

.. toctree::
   :maxdepth: 2

Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`

Background
==========

Mobile devices promise to keep users connected.  Yet, limited energy,
data-transfer allowances, and cellular coverage reveal this assurance
to be more a hope than a guarantee.  This situation can be improved by
increasing battery capacity, providing more generous data-transfer
allowances, and expanding cellular coverage.  We propose an
alternative: modifying software to more efficiently use the available
resources.  In particular, many applications exhibit flexibility in
when they must transfer data.  For example, podcast managers can
prefetch podcasts and photo sharing services can delay uploads until
good conditions arise.  More generally, applications that operate on
data streams often have significant flexibility in when they update
the stream.

More efficiently managing the available energy, the user's
data-transfer allowance and data availability can improve the user
experience.  Increasing battery life raises the user's confidence that
a charge will last the whole day, even with intense use.
Alternatively, a smaller battery can be used decreasing the device's
monetary cost as well as its weight and size.  Explicitly managing the
data-transfer allowance enables users to choose less expensive data
plans without fearing that the allowance will be exceeded, which may
result in expensive overage fees and bill shock, a common occurrence
in the US.  Finally, accounting for availability by, e.g., prefetching
data, hides spotty and weak network coverage and user-perceived
latency is reduced.

We have encountered two main challenges to exploiting scheduling
flexibility in data streams: predicting needed data and coordinating
resource consumption.  First, applications need to predict when and
what to prefetch.  Consider Alice, who listens to the latest episode
of the hourly news on her 5 PM commute home.  A simple policy
prefetches episodes as they are published.  As Alice only listens to
the 4 PM or 5 PM episode, downloading episodes as they are published
wastes energy and her data-transfer allowance.  An alternative policy
prefetches when power and WiFi are available, typically overnight.
But, Alice wants the latest episode on her commute home, not the one
from 6 AM.  The scheduling algorithm needs to learn when and how Alice
(the individual, not an aggregate model) uses data streams.  The
second challenge is coordinating the use of available resources.  In
particular, the data-transfer allowance and local storage must be
partitioned between the applications and the user.  This management
should not interfere with the user by, e.g., exhausting the transfer
allowance so that the user cannot surf the web, or causing an
out-of-space error to occur when the user saves files.  Further, the
allocations should adapt to the user's changing preferences.

Research on scheduling transmissions on smart phones has focused on
reducing energy consumption by predicting near-term conditions.
Bartendr delays transmissions until the signal strength is likely
strong; TailEndr groups transmissions to amortizes energy costs;
BreadCrumbs, among others, predicts WiFi availability to reduce energy
spent needlessly scanning.  contextual deadline, as predicted from
observed user behavior.  We also consider the cellular
data-transmission allowance, which is an increasingly common
constraint.  Further, because we enable aggressive prefetching, we
consider how to manage storage.  Given multimedia data access patterns
in which subscribed to data is used at most once, common replacement
techniques, such as LRU, perform poorly.

In short, Woodchuck enables better scheduling of background
data-stream updates to save energy, to make better use of
data-transfer allowances, to improve disconnected operation, and to
hide data-access latencies, all of which advance our ultimate goal of
improving the user experience.  To use Woodchuck, applications provide
simple descriptions of transmission tasks.  Woodchuck uses these and
*predictions* of when, where and how data will be used based on
application-input and historical data as well as when streams will be
updated to schedule the requests so as to minimize battery use, to
respect any data-transmission allowance, and to maximize the
likelihood that data that the user accesses is available.  We also
consider how to *manage storage* for holding prefetched data.

Programming Model
=================

Before detailing Woodchuck's API, we provide a brief introduction to
Woodchuck's main concepts and some case studies of how we envision
some applications could exploit Woodchuck.

Woodchuck's model is relatively simple: there are managers, streams
and objects.  A manager represents an application (e.g., a podcast
client).  It contains streams.  A stream represents a data source
(e.g., a podcast feed).  It references objects.  An object represents
some chunk of data (e.g., a podcast).  Typically, users explicitly
subscribe to a stream.  The stream are regularly updated to discover
new objects, which may be downloaded when convenient.

Managing the various object types is straightforward.  An application
registers a manager by calling ManagerRegister.  Given a manager, the
application registers streams using manager.StreamRegister.
Similarly, an application registers objects with a stream using
stream.ObjectRegister.

Associated with each object (Manager, Stream or Object) is a UUID and
a cookie.  The UUID uniquely identifies the object.  The cookie is a
free-form string that is uninterpreted by Woodchuck.  It can be used
by an application to store a database key or URL.  This appears to
greatly simplify the changes to the application as it eliminates the
need for the application to manage a map between Woodchuck's UUIDs and
local stream and object identifiers.

Woodchuck makes an upcall, StreamUpdate and ObjectDownload, to the
application when the application should update a stream or download an
object, respecitvely.  After updating a stream, the application
invokes stream.UpdateStatus and registers any newly discovered objects
using stream.ObjectRegister.  ObjectDownload tells the application to
download an object.  After attempting the download, the application
responds by calling object.DownloadStatus.

When a user uses an object, an application can report this to
Woodchuck using object.Used.  The application can include a bitmask
representing the portions of the object that were used.  This assumes
that there is some serial representation as is the case with videos
and books.

When space becomes scarce, Woodchuck can delete files.  When an
application registers an object, it can include a deletion policy,
which indicates whether the object is precious and may only be deleted
by the user, whether Woodchuck may delete it without consulting the
application, or whether to ask the application to delete the object.
In the last case, Woodchuck uses the ObjectDeleteFiles upcall.  The
application responds using Object.FilesDeleted indicating either: the
object has been deleted; the object should be preserved for at least X
seconds longer; or, the object has been shrunk.  Shrinking an object
is useful for data like email where an email's bulky attachments can
be purged while still retaining the body.

One thing that I have not yet considered is an interface to allow
applications to implement custom deletion policies.  Although an
application can delete a file at any file and communicate this to
Woodchuck using the object.FilesDeleted interface, there is currently
no mechanism for an application to say: "Tell me when there is storage
pressure and I'll find the best files to delete."


Case Studies
------------

To evaluate the applicability of the model, I've been using a few case
studies: podcasts, blogs, weather and package repository updates.
(Email and calendaring are similar to podcasts.  Social networking
(facebook, twitter, flickr) appears to be hybrid of podcasts and
blogs.)

Podcast Manager
^^^^^^^^^^^^^^^

A podcast manager fits the proposed model very well.  The podcast
application registers one stream for each podcast subscription.  When
it updates a stream, it registers each new podcast episode as a
Woodchuck object.  When a podcast is viewed or listened to, it is easy
to determine which parts were used.

Blog Reader
^^^^^^^^^^^

A blog reader is similar to the podcast application: a subscription
cleanly maps to Woodchuck's stream concept and articles to Woodchuck's
object.  Unlike the podcast application, new objects are typically
transferred inline as part of a stream update.  That is, a stream
update consists not of an enumeration of new objects and references,
but the objects' contents.  When such an application updates a stream,
it registers new objects as usual and also marks them as having been
downloaded.

It should be relatively easy for Woodchuck to detect that the objects
were delivered inline: the download time is the same as the stream
update time.  Nevertheless, I've exposed a stream property named
stream.ObjectsMostInline, which an application can set if it expects
this behavior.

Determining use for the application is also relatively
straightforward: when an article is viewed, it has been used.  It is
possible to infer partial use for longer articles where scrolling is
required.  If the blog reader displays blogs using a continuous reader
(like Google Reader), then this won't work, but it is still possible
for the application to infer use based on how fast the user scrolls.

Package Repository Updates
^^^^^^^^^^^^^^^^^^^^^^^^^^

At first glance, managing a package repository looks like managing
podcasts.  Unlike the podcast manager, prefetching most applications
is useless: few users install more than dozens of applications.  The
few packages it makes sense to prefetch are updates to installed
packages.  Woodchuck can't distinguish these on its own.  It is
possible to teach Woodchuck this by way of object's priority property
(*org.woochuck.object.Priority*).  The application manager would then
set this to high (e.g., 10) for packages that are installed and low
(e.g., 1) for packages that are not installed.  Woodchuck learns to
trust the application based on actual use.

An alternative, planned approach is to provide a mechanism that allows
applications to implement their own scheduling strategy.  This can be
down by having Woodchuck make an upcall indicating that the
application should fetch the X MBs of most useful data.

If it turns out there are too many packages, just register those for
which prefetching makes sense.  But, always report the number of
actually downloaded packages when calling
*org.woochuck.stream.UpdateStatus*.

Weather
^^^^^^^

The weather application is quite different from the podcast and blog
applications.  Most people, I think, are interested in monitoring a
few locations at most, e.g., Baltimore and San Jose.  In this case,
the stream is not a series of immutable objects, but a series of
object updates for a single object.

The best approach is to represent weather updates as a stream.
Updating the stream means getting the latest weather.  But then, the
stream appears to have no objects.  How do we track use?  What about
publication time?  One solution is that after each update, the
application creates a new object and marks it as having been
downloaded.  The application should not register missed updates.
Mostly likely it doesn't even know how frequently the weather is
updated.  To indicate that a new update is available, create a new
object.  If the update is only available in the future, set the
object's TriggerEarliest property appropriately
(*org.woochuck.object.TriggerEarliest*).

DBus Interface
==============

Woodchuck exposes its functionality via DBus.  Applications, however,
do not need to use this low-level interface.  Instead, there is a C
library that wraps Woodchuck's functionality and Python modules.
**Application developers can ignore this section** and read just about
the interface they are interested in and only refer to this chapter
for additional details, as required.

.. toctree::

  org.woodchuck
  org.woodchuck.manager
  org.woodchuck.stream
  org.woodchuck.object
  org.woodchuck.upcall

C Library
=========

The C library provides a more convenient interface to access
Woodchuck's functionality than the low-level DBus interface.  To do
so, it makes a few assumption about how the streams and objects are
managed.  In particular, it assumes that a single application uses the
specified manager and that it does so in a particular way.  First, it
assumes that the application only uses a top-level manager;
hierarchical managers are not supported.  It also assumes that streams
and objects are uniquely identified by their respective cookies
(thereby allowing the use of
:func:`org.woodchuck.LookupManagerByCookie`).  For most applications,
these limitations should not present a burden.

The C library currently only works with programs using the `glib`_
mainloop and the gobject object system.

The C library is currently only documented in the header files
<`woodchuck/woodchuck.h`_> and <`woodchuck/gwoodchuck.h`_>.  Please
refer to it for reference.  Note, however, that the interface is very
similar to the :class:`PyWoodchuck` interface.

.. _glib: http://developer.gnome.org/glib/

.. _woodchuck/woodchuck.h: http://hssl.cs.jhu.edu/~neal/woodchuck/src/branches/master/include/woodchuck/woodchuck.h.raw.html

.. _woodchuck/gwoodchuck.h: http://hssl.cs.jhu.edu/~neal/woodchuck/src/branches/master/include/woodchuck/gwoodchuck.h.raw.html



Python Modules
==============

There are two python modules for interacting with a Woodchuck server:
*pywoodchuck* and *woodchuck*.  *pywoodchuck* is a high-level module,
which provides a Pythonic interface.  It hides a fair amount of
complexity while sacrificing only a small amount of functionality.  It
is recommended for most applications.  The *woodchuck* module is a
thin wrapper on top of the DBus interface.

pywoodchuck
-----------

.. currentmodule:: pywoodchuck

The :mod:`pywoodchuck` module provides a high-level Pythonic
interface to Python.

PyWoodchuck
^^^^^^^^^^^

.. autoclass:: PyWoodchuck
    :members: available, stream_register, streams_list, stream_updated,
        stream_update_failed, stream_unregister, object_register,
        objects_list, object_unregister, object_downloaded,
        object_download_failed, object_used, object_files_deleted,
	stream_property_get, stream_property_set,
	object_property_get, object_property_set,
        object_downloaded_cb, stream_update_cb, object_download_cb,
        object_delete_files_cb

.. autoclass:: _Stream
    :members: unregister, updated, update_failed, object_register,
        objects_list, object_downloaded, object_download_failed,
	object_files_deleted

.. autoclass:: _Object
    :members: unregister, downloaded, download_failed, used, files_deleted

Constants
^^^^^^^^^

.. autodata:: pywoodchuck.never_updated


woodchuck
---------

.. module:: woodchuck

The :mod:`woodchuck` module is a low-level wrapper of the DBus
interface.  Each of Woodchuck's object types is mirrored by a
similarly named Python class.

The :mod:`woodchuck` module uses a factory for managing instantiations
of the objects.  In particular, the factory ensures that there is at
most one Python object per Woodchuck object.  That is, the same Python
object is shared by all users of a given Woodchuck object.

Woodchuck
^^^^^^^^^

The Woodchuck object wraps the top-level Woodchuck interface.

.. autofunction:: Woodchuck

.. autoclass:: _Woodchuck
    :members: manager_register, list_managers, lookup_manager_by_cookie

Manager
^^^^^^^

The *_Manager* class wraps a Woodchuck manager.

.. autofunction:: Manager

.. autoclass:: _Manager
    :members:

Stream
^^^^^^

.. autoclass:: woodchuck._Stream
    :members:

Object
^^^^^^

.. autoclass:: woodchuck._Object
    :members:

Upcall
^^^^^^

.. autoclass:: woodchuck.Upcalls
    :members: object_downloaded_cb, stream_update_cb,
        object_download_cb, object_delete_files_cb

Constants
^^^^^^^^^

.. autoclass:: woodchuck.RequestType
   :members: UserInitiated, ApplicationInitiated

.. autoclass:: woodchuck.DownloadStatus
   :members:

.. autoclass:: woodchuck.Indicator
   :members:

.. autoclass:: woodchuck.DeletionPolicy
   :members:

.. autoclass:: woodchuck.DeletionResponse
   :members:

Exceptions
^^^^^^^^^^

.. autoexception:: woodchuck.Error

.. autoexception:: woodchuck.GenericError

.. autoexception:: woodchuck.NoSuchObject

.. autoexception:: woodchuck.ObjectExistsError

.. autoexception:: woodchuck.NotImplementedError

.. autoexception:: woodchuck.InternalError

.. autoexception:: woodchuck.InvalidArgsError

.. autoexception:: woodchuck.UnknownError

.. autoexception:: woodchuck.WoodchuckUnavailableError


