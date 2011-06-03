org.woodchuck.upcall
--------------------

.. class:: org.woodchuck.upcall

    .. function:: ObjectDownloaded (ManagerUUID, ManagerCookie, StreamUUID, StreamCookie, ObjectUUID, ObjectCookie, Status, Instance, Version, Filename, Size, TriggerTarget, TriggerFired)

        Upcall from Woodchuck indicating that a download has
        completed.  After processing, the application should
        acknowledge the feedback using FeedbackACK, otherwise, it
        will be resent.  

        :param in ManagerUUID s:
            The manager's UUID.  

        :param in ManagerCookie s:
            The manager's cookie.  

        :param in StreamUUID s:
            The stream's UUID.  

        :param in StreamCookie s:
            The stream's cookie.  

        :param in ObjectUUID s:
            The object's UUID.  

        :param in ObjectCookie s:
            The object's cookie.  

        :param in Status u:
            Whether the download was successful or not.  See the status
            argument of :func:`org.woodchuck.object.DownloadStatus` for
            the possible values.  

        :param in Instance u:
            The number of download attempts (not including this one).
            
            This is the instance number of the feedback.  

        :param in Version ustub:
            Index and value of the version downloaded from the versions
            array (at the time of download).  See
            :data:`org.woodchuck.object.Versions`.  

        :param in Filename s:
            The location of the data.  

        :param in Size t:
            The size (in bytes).  

        :param in TriggerTarget t:
            The target time.  

        :param in TriggerFired t:
            The time at which the download was attempted.  


    .. function:: StreamUpdate (ManagerUUID, ManagerCookie, StreamUUID, StreamCookie)

        Update the specified stream.
        
        Respond by calling
        :func:`org.woodchuck.stream.UpdateStatus`. 

        :param in ManagerUUID s:
            The manager's UUID.  

        :param in ManagerCookie s:
            The manager's cookie.  

        :param in StreamUUID s:
            The stream's UUID.  

        :param in StreamCookie s:
            The stream's cookie.  


    .. function:: ObjectDownload (ManagerUUID, ManagerCookie, StreamUUID, StreamCookie, ObjectUUID, ObjectCookie, Version, Filename, Quality)

        Download the specified object.
        
        Respond by calling
        :func:`org.woodchuck.object.DownloadStatus`. 

        :param in ManagerUUID s:
            The manager's UUID.  

        :param in ManagerCookie s:
            The manager's cookie.  

        :param in StreamUUID s:
            The stream's UUID.  

        :param in StreamCookie s:
            The stream's cookie.  

        :param in ObjectUUID s:
            The object's UUID.  

        :param in ObjectCookie s:
            The object's cookie.  

        :param in Version (ustub):
            Index and value of the version to download from the
            versions array (at the time of the upcall).  See
            :data:`org.woodchuck.object.Versions`. 

        :param in Filename s:
            The value of :data:`org.woodchuck.object.Filename`.  

        :param in Quality u:
            Target quality from 1 (most compressed) to 5 (highest
            available fidelity).  This is useful if all possible
            versions cannot be or are not easily expressed by the
            Version parameter.  


    .. function:: ObjectDeleteFiles (ManagerUUID, ManagerCookie, StreamUUID, StreamCookie, ObjectUUID, ObjectCookie, Files)

        Delete the files associated with the specified object.
        Respond by calling
        :func:`org.woodchuck.object.FilesDeleted`. 

        :param in ManagerUUID s:
            The manager's UUID.  

        :param in ManagerCookie s:
            The manager's cookie.  

        :param in StreamUUID s:
            The stream's UUID.  

        :param in StreamCookie s:
            The stream's cookie.  

        :param in ObjectUUID s:
            The object's UUID.  

        :param in ObjectCookie s:
            The object's cookie.  

        :param in Files a(sbu):
            The list of files associated with this object, as provided
            the call to :func:`org.woodchuck.object.DownloadStatus`.
            


