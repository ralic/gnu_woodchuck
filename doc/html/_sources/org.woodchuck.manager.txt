org.woodchuck.manager
---------------------

.. class:: org.woodchuck.manager

    Object: /org/woodchuck/manager/`ManagerUUID` 

    .. function:: Unregister (OnlyIfNoDescendents)

        Unregister this manager and any descendent objects.  This
        does not remove any files; only the metadata stored on the
        Woodchuck server is deleted.  

        :param in OnlyIfNoDescendents b:
            If true, fail if this manager has any descendents.  


    .. function:: ManagerRegister (Properties, OnlyIfCookieUnique, UUID)

        Register a new manager, which is subordinate to this one.
        
        This enables the creation of a manager hierarchy, which is
        useful for separating a program's components.  For instance,
        a web browser might have a page cache and a set of files that
        should be downloaded later.  Each should be registered as a
        child manager to the top-level web browser manager.  

        :param in Properties a{sv}:
            Dictionary of initial values for the various
            properties.
            
            The following properties are required: `HumanReadableName`.
            
            Note: The a{ss} type is also supported, but then only
            properties with a string type may be expressed.  (This is a
            concession to dbus-send, as it does not support parameters
            with the variant type.)
            

        :param in OnlyIfCookieUnique b:
            Only succeed if the supplied cookie is unique among all
            sibling managers.  

        :param out UUID s:
            The new manager's unique identifier (a 16-character
            alpha-numeric string).  


    .. function:: ListManagers (Recursive, Managers)

        Return a list of child managers.  

        :param in Recursive b:
            Whether to list all descendents (true) or just immediate
            children (false).  

        :param out Managers a(ssss):
            An array of <`UUID`, `Cookie, `HumanReadableName`,
            `ParentUUID`>.  


    .. function:: LookupManagerByCookie (Cookie, Recursive, Managers)

        Return the managers whose `Cookie` property matches the
        specified cookie.  

        :param in Cookie s:
            The cookie to match.  

        :param in Recursive b:
            If true, consider any descendent manager.  If false, only
            consider immediate children.  

        :param out Managers a(sss):
            An array of <`UUID`, `HumanReadableName`, `ParentUUID`>.  


    .. function:: StreamRegister (Properties, OnlyIfCookieUnique, UUID)

        Register a new stream.  

        :param in Properties a{sv}:
            Dictionary of initial values for the various
            properties. See the :class:`org.woodchuck.stream`
            interface for the list of properties and their meanings.
            
            The following properties are required: `HumanReadableName`
            
            Note: The a{ss} type is also supported, but then only
            properties with a string type may be expressed.  (This is a
            concession to dbus-send, as it does not support parameters
            with the variant type.)  

        :param in OnlyIfCookieUnique b:
            Only succeed if the supplied cookie is unique among all
            streams belonging to this manager.  

        :param out UUID s:
            The new stream's unique identifier.  


    .. function:: ListStreams (Streams)

        Return a list of streams.  

        :param out Streams a(sss):
            An array of <UUID, COOKIE, HUMAN_READABLE_NAME>.  


    .. function:: LookupStreamByCookie (Cookie, Streams)

        Return a list of streams with the cookie COOKIE.  

        :param in Cookie s:
            The cookie to match.  

        :param out Streams a(ss):
            An array of <`UUID`, `HumanReadableName`>.  


    .. function:: FeedbackSubscribe (DescendentsToo, Handle)

        Indicate that the calling process would like to receive
        upcalls pertaining to this manager and (optionally) any of
        its descendents.
        
        .. Note: Upcalls are sent to all subscriptions.  Thus, if a
            single process has multiple subscriptions, it will receive
            the same upcall multiple times.
        
        Feedback is sent until :func:`FeedbackUnsubscribe` is called.
        
        .. Note: If the calling process's private DBus name becomes
            invalid, the subscription is automatically cancelled.
        

        :param in DescendentsToo b:
            If true, also make upcalls for any descendents.  

        :param out Handle s:
            An opaque handle, that must be passed to
            :func:`FeedbackUnSubscribe`.  


    .. function:: FeedbackUnsubscribe (Handle)

        Request that Woodchuck cancel the indicated subscription.
        

        :param in Handle s:
            The handle returned by :func:`FeedbackSubscribe`.  


    .. function:: FeedbackAck (ObjectUUID, ObjectInstance)

        Ack the feedback with the provided UUID.  

        :param in ObjectUUID s:

        :param in ObjectInstance u:


    .. data:: ParentUUID

        This manager's parent manager.  

    .. data:: HumanReadableName

        A human readable name for the manager.  When displaying a
        manager's human readable name, the human readable name of
        each of its ancestors as well as its own will be concatenated
        together.  Thus, if the manager's parent is called "Firefox"
        and it has a child web cache, the human readable name of the
        child should be "Web Cache," not "Firefox Web Cache."  The
        latter would result in "Firefox Firefox Web Cache" being
        displayed to the user.  

    .. data:: Cookie

        A free-form string uninterpreted by the server and passed to
        any manager upcalls.
        
        By convention, this is set to the application's DBus name
        thereby allowing all application's to easily lookup the UUID
        of their manager and avoiding any namespace collisions.
        
        .. Note: Woodchuck does not enforce that the `Cookie`
            property be unique.  

    .. data:: DBusServiceName

        The DBus service name of the service to start when there is
        work to do, e.g., streams to update or objects to download.
        See :class:`org.woodchuck.upcall`.  

    .. data:: DBusObject

        The DBus object to send upcalls to.  This defaults to
        '/org/woodchuck'.  

    .. data:: Priority

        The priority, relative to other managers with the same parent
        manager.  

