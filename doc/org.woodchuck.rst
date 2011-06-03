org.woodchuck
-------------

.. class:: org.woodchuck

    The top-level interface to Woodchuck.
    
    By default, Woodchuck listens on the session bus.  It registers
    the DBus service name `org.woodchuck` and uses the object
    `/org/woodchuck`.
    

    .. function:: ManagerRegister (Properties, OnlyIfCookieUnique, UUID)

        Register a new manager.
        
        Also see the :func:`org.woodchuck.manager.ManagerRegister`.
        
        :returns: The UUID of a new manager object.  Manipulate the
            manager object using :class:`org.woodchuck.manager`
            interface and the object /org/woodchuck/manager/`UUID`.  

        :param in Properties a{sv}:
            Dictionary of initial values for the various
            properties. See the :class:`org.woodchuck.manager`
            interface for the list of properties and their meanings.
            
            The following properties are required: `HumanReadableName`
            
            Note: The a{ss} type is also supported, but then only
            properties with a string type may be expressed.  (This is a
            concession to dbus-send, as it does not support parameters
            with the variant type.)  

        :param in OnlyIfCookieUnique b:
            Only succeed if the supplied cookie is unique among all
            top-level managers.  

        :param out UUID s:
            The new manager's unique identifier (a 16-character
            alpha-numeric string).  


    .. function:: ListManagers (Recursive, Managers)

        Return a list of the known managers.  

        :param in Recursive b:
            Whether to list all descendents (true) or just top-level
            manager (false).  

        :param out Managers a(ssss):
            An array of <`UUID`, `Cookie, `HumanReadableName`,
            `ParentUUID`>.  


    .. function:: LookupManagerByCookie (Cookie, Recursive, Managers)

        Return the managers whose `Cookie` property matches the
        specified cookie.  

        :param in Cookie s:
            The cookie to match.  

        :param in Recursive b:
            If true, consider any manager.  If false, only consider
            top-level managers.  

        :param out Managers a(sss):
            An array of <`UUID`, `HumanReadableName`, `ParentUUID`>.  


    .. function:: DownloadDesirability (RequestType, Versions, Desirability, Version)

        Evaluate the desirability of executing a download right now.  

        :param in RequestType u:
            The type of request:
            
            * 1: User initiated
            * 2: Application initiated
            

        :param in Versions a(tu):
            Array of <`ExpectedSize`, `Utility`> tuples.  See
            :data:`org.woodchuck.object.Versions` for a description.  

        :param out Desirability u:
            
            The desirability of executing the job now:
            
            * 0: Avoid if at all possible.
            * 5: Now is acceptable but waiting is better.
            * 9: Now is ideal.
            

        :param out Version u:
            The version to download as an index into the passed
            Versions array.  -1 means do not download anything.  


