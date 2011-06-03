org.woodchuck.stream
--------------------

.. class:: org.woodchuck.stream

    Object: /org/woodchuck/stream/`StreamUUID`. 

    .. function:: Unregister (OnlyIfEmpty)

        Unregister this stream and any descendent objects.  This does
        not remove any files, only metadata stored on the Woodchuck
        server is deleted.  

        :param in OnlyIfEmpty b:
            If true, fail if this stream has any registered objects.
            


    .. function:: ObjectRegister (Properties, OnlyIfCookieUnique, UUID)

        Register a new object.  

        :param in Properties a{sv}:
            Dictionary of initial values for the various
            properties. See the :class:`org.woodchuck.object` interface
            for the list of properties and their meanings.
            
            No properties are required.
            
            Note: The a{ss} type is also supported, but then only
            properties with a string type may be expressed.  (This is a
            concession to dbus-send, as it does not support parameters
            with the variant type.)  

        :param in OnlyIfCookieUnique b:
            Only succeed if the supplied cookie is unique among all
            objects in this stream.  

        :param out UUID s:
            The new object's unique identifier.  


    .. function:: ListObjects (Objects)

        Return a list of objects in this stream.  

        :param out Objects a(sss):
            An array of <`UUID`, `Cookie, `HumanReadableName`,
            `ParentUUID`>.  


    .. function:: LookupObjectByCookie (Cookie, Objects)

        Return the objects whose `Cookie` property matches the
        specified cookie.  

        :param in Cookie s:
            The cookie to match.  

        :param out Objects a(ss):
            An array of <`UUID`, `HumanReadableName`>.  


    .. function:: UpdateStatus (Status, Indicator, TransferredUp, TransferredDown, DownloadTime, DownloadDuration, NewObjects, UpdatedObjects, ObjectsInline)

        Indicate that a stream has been downloaded.
        
        This is typically called in reaction to a
        :func:`org.woodchuck.upcall.StreamUpdate` upcall, but should
        whenever a stream is updated.
        

        :param in Status u:
            0: Success.
            
            Transient errors (will try again later):
            
            * 0x100: Other.
            * 0x101: Unable to contact server.
            * 0x102: Transfer incomplete.
            
            Hard errors (give up trying to download this object):
            
            * 0x200: Other.
            * 0x201: File gone.
            

        :param in Indicator u:
            The type of indicator displayed to the user, if any.  A
            bitmask of:
            
              * 0x1: Audio sound
              * 0x2: Application visual notification
              * 0x4: Desktop visual notification, small, e.g., blinking icon
              * 0x8: Desktop visual notification, large, e.g., system tray
                notification
              * 0x10: External visible notification, e.g., an LED
              * 0x20: Vibrate
            
              * 0x40: Object-specific notification
              * 0x80: Stream-wide notification, i.e., an aggregate
                notification for all updates in the stream.
            
              * 0x100: Manager-wide notification, i.e., an aggregate
                notification for all updates in the manager.
            
              * 0x80000000: It is unknown if an indicator was shown.
            
            0 means that no notification was shown.
            

        :param in TransferredUp t:
            The approximate number of bytes uploaded.  If unknown, pass
            -1.  

        :param in TransferredDown t:
            The approximate number of bytes downloaded.  If unknown,
            pass -1.  

        :param in DownloadTime t:
            The time at which the download was started (in seconds
            since the epoch).  Pass 0 if unknown. 

        :param in DownloadDuration u:
            The time, in seconds, it took to perform the download.
            Pass 0 if unknown.  

        :param in NewObjects u:
            The number of new objects discovered.  If not known, pass
            -1.  

        :param in UpdatedObjects u:
            The objects discovered to have changes.  If not known, pass
            -1.  

        :param in ObjectsInline u:
            The number of inline updates.  If not known, pass -1.  


    .. data:: ParentUUID

        The manager this streams belongs to.  

    .. data:: HumanReadableName

        A human readable name for the stream.  When displaying a
        stream's human readable name, it will always be displayed
        with the human readable name of the manager.  

    .. data:: Cookie

        A free-form string uninterpreted by the server and passed to
        any stream upcalls.
        
        The application can set this to a database key or URL to
        avoid having to manage a mapping between Woodchuck UUIDs and
        local identifiers.  

    .. data:: Priority

        The priority, relative to other streams managed by the same
        manager.  

    .. data:: Freshness

        How often the stream should be updated, in seconds.
        
        A value of UINT32_MAX is interpretted as meaning that the
        stream is never updated, in which case, there is no need to
        check for stream updates.  

    .. data:: ObjectsMostlyInline

        Whether objects are predominantly inline (i.e., delivered
        with stream updates) or not.  Default: False.
        
        Consider an RSS feed for a blog: this often includes the
        article text.  This is unlike a Podcast feed, which often
        just includes links to the objects' contents.  

