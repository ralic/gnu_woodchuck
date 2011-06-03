org.woodchuck.object
--------------------

.. class:: org.woodchuck.object

    Object: /org/woodchuck/object/`ObjectUUID` 

    .. function:: Unregister ()

        Unregister this object.  This does not remove any files, only
        metadata stored on the Woodchuck server is deleted.  


    .. function:: Download (RequestType)

        This object is needed, e.g., the user just select an email to
        read.
        
        This method is only useful for object's that make use of
        Woodchuck's simple downloader.  See
        :data:`org.woodchuck.object.Versions` for more information.
        

        :param in RequestType u:
            The type of request.
            
            * 1 - User initiated
            * 2 - Application initiated
            


    .. function:: DownloadStatus (Status, Indicator, TransferredUp, TransferredDown, DownloadTime, DownloadDuration, ObjectSize, Files)

        Indicate that an object has been downloaded.
        
        This is typically called in reaction to a
        :func:`org.woodchuck.upcall.ObjectDownload` upcall, but
        should whenever an object is downloaded.
        
        The value of the object's :data:`Instance` property will be
        incremented by 1.
        

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
            The approximate number of bytes uploaded.  (Pass -1 if
            unknown.)  

        :param in TransferredDown t:
            The approximate number of bytes downloaded.  (Pass -1 if
            unknown.)  

        :param in DownloadTime t:
            The time at which the download was started (in seconds
            since the epoch).  Pass 0 if unknown. 

        :param in DownloadDuration u:
            The time, in seconds, it took to perform the download.
            Pass 0 if unknown.  

        :param in ObjectSize t:
            The size of the object on disk (in bytes).  Pass -1 if
            unknown.  

        :param in Files a(sbu):
            An array of <`Filename`, `Dedicated`, `DeletionPolicy`>
            tuples.
            
            `Filename` is the absolute filename of a file that contains
            data from this object.
            
            `Dedicated` indicates whether `Filename` is dedicated to
            that object (true) or whether it includes other state
            (false).
            
            `DeletionPolicy` indicates if the file is precious and may
            only be deleted by the user (0), if the file may be deleted
            by woodchuck without consulting the application (1), or if
            the application is willing to delete the file (via
            :func:`org.woodchuck.upcall.ObjectDeleteFiles`) (2).  


    .. function:: Used (Start, Duration, UseMask)

        :param in Start t:
            When the user started using the object.  

        :param in Duration t:
            How long the user used the object.  -1 means unknown.  0
            means instantaneous.  

        :param in UseMask t:
            Bit mask indicating which portions of the object were used.
            Bit 0 corresponds to the first 1/64 of the object, bit 1 to
            the second 1/64 of the object, etc.  


    .. function:: FilesDeleted (Update, Arg)

        Call when an objects files have been removed or in response
        to org.woodchuck.upcall.ObjectDelete.  

        :param in Update u:
            Taken from enum woodchuck_delete_response (see
            <woodchuck/woodchuck.h>):
            
            * 0: Files deleted.  ARG is ignored.
            * 1: Deletion refused.  Preserve for at least ARG
              seconds before asking again.
            * 2: Files compressed.  ARG is the new size in bytes.
              (-1 = unknown.)
            

        :param in Arg t:


    .. data:: ParentUUID

        The stream this object belongs to.  

    .. data:: Instance

        The number of times this object has been downloaded.  

    .. data:: HumanReadableName

        A human readable name.  

    .. data:: Cookie

        Uninterpretted by Woodchuck.  This is passed in any
        object upcalls.
        
        The application can set this to a database key or URL to
        avoid having to manage a mapping between Woodchuck UUIDs and
        local identifiers.  

    .. data:: Versions

        An array of <`URL`, `ExpectedSize`, `Utility`,
        `UseSimpleDownloader`> tuples.  Each tuple designates the
        same object, but with a different quality.
        
        `URL` is optional.  Its value is only interpretted by
        Woodchuck if `UseSimpleDownloader` is also true.
        
        `ExpectedSize` is the expected transfer size, in bytes.
        
        `Utility` is the utility of this version of the object
        relative to other versions of this object.  Woodchuck
        interprets the value linearly: a version with twice the
        utility is consider to offer twice the quality.  If bandwidth
        is scarce but the object is considered to have a high
        utility, a lower quality version may be downloaded.  If a
        version has no utility, then it shouldn't be listed here.
        
        `UseSimpleDownloader` specifies whether to use Woodchuck's
        built in simple downloader for downloading this object.  When
        Woodchuck has downloaded an object, it will invoke the
        :func:`org.woodchuck.upcall.ObjectDownloaded` upcall.
        
        If `UseSimpleDownloader` is false, Woodchuck will make the
        :func:`org.woodchuck.upcall.ObjectDownload` upcall to the
        application when the application should download the object.
        Woodchuck also specified which version of the object to
        download.
        

    .. data:: Filename

        Where to save the file(s).  If FILENAME ends in a /,
        interpreted as a directory and the file is named after the
        URL.  

    .. data:: Wakeup

        Whether to wake the application when this job completes
        (i.e., by sending a dbus message) or to wait until a process
        subscribes to feedback (see
        :func:`org.woodchuck.manager.FeedbackSubscribe`).  This is
        only meaningful if the Woodchuck server downloads the file
        (i.e., `UseSimpleDownloader` is true).
        

    .. data:: TriggerTarget

        Approximately when the download should be performed, in
        seconds since the epoch.  (If the property Period is not
        zero, automatically updated after each download.)
        
        The special value 0 means at the next available opportunity.
        

    .. data:: TriggerEarliest

        The earliest time the download may occur.  Seconds prior to
        TriggerTarget.  

    .. data:: TriggerLatest

        The latest time the download may occur.  After this time, the
        download will be reported as having failed.
        
        Seconds after TriggerTarget.  

    .. data:: DownloadFrequency

        The period (in seconds) with which to repeat this download.
        Set to 0 to indicate that this is a one-shot download.  This
        is useful for an object which is updated periodically, e.g.,
        the weather report.  You should not use this for a
        self-contained stream such as a blog.  Instead, on
        downloading the feed, register each contained story as an
        individual object and mark it as downloaded immediately.
        Default: 0.  

    .. data:: Priority

        The priority, relative to other objects in the stream.  

    .. data:: DiscoveryTime

        The time at which the object was discovered (in seconds since
        the epoch).  This is normally the time at which the stream
        was updated.  

    .. data:: PublicationTime

        The time at which the object was published (in seconds since
        the epoch).  

