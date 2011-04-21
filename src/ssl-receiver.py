# ssl-receiver.py - Smart storage logger upload server.
# Copyright (C) 2009, 2010, 2011 Neal H. Walfield <neal@walfield.org>
#
# Smart storage is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 3, or (at
# your option) any later version.
#
# Smart storage is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.

# For enabling HTTPS: http://code.activestate.com/recipes/442473-simple-http-server-supporting-ssl-secure-communica/
# To generate a key, use:
#  openssl req -new -x509 -keyout ssl-receiver.key -out ssl-receiver.cert -days 3650 -nodes

# You need to run this in the same directory as the SSL keys (in the
# source tree, that is data).  Uploaded data will be saved to the
# upload.db database in the current directory.

import socket
import BaseHTTPServer
import httplib
import http_header
import sys
import SocketServer
import threading
import sqlite3
import OpenSSL
import os
import base64

do_debug = 1
def debug (*args):
    if do_debug:
        for i in args:
            sys.stdout.write (i)
        sys.stdout.write ('\n')


class uploader_handler (BaseHTTPServer.BaseHTTPRequestHandler):
    """
    A smart storage logger uploader handler.
    """
    def __init__ (self, *args):
        # The mimetools.Message class is annoying.  For instance, it
        # does not do proper unfolding, and indexing only returns a
        # single header value, not all the value of all headers with
        # that name combined.  Override it.
        self.MessageClass = http_header.http_headers
        try:
            BaseHTTPServer.BaseHTTPRequestHandler.__init__ (self, *args)
        except OpenSSL.SSL.Error, err:
            self.log_error ("SSL error: %s", str (err))

    def setup(self):
        self.connection = self.request
        self.log_message ("Client's certificate: %s",
                          str (self.connection.get_peer_certificate()));
        self.rfile = socket._fileobject(self.request, "rb", self.rbufsize)
        self.wfile = socket._fileobject(self.request, "wb", self.wbufsize)

    def do_POST (self):
        try:
            if self.headers.get ('expect', '') == '100-continue':
                self.send_response (100, 'Continue')
    
            try:
                l = int (self.headers['content-length'])
            except:
                l = None
            if l is not None:
                # Since we are a 1.0 server, we should not get chunk
                # encoded data.
                content = self.rfile.read (l)
            else:
                raise self.respond (411, msg='content-length not provided.')
    
            debug ("Length: ", str (l), "; got: ", str (len (content)));
    
            conn = sqlite3.connect ('uploads.db', timeout=60*60);
            cur = conn.cursor ()
            try:
                cur.execute ('create table if not exists uploads'
                             ' (uuid, date, headers, content);');
            except sqlite3.OperationalError:
                # Table already exists...
                pass
    
            conn.commit ()
            cur.execute ('insert into uploads'
                         ' values (?, strftime("%s","now"), ?, ?)',
                         (self.path, str (self.headers),
                          base64.encodestring (content)));
            conn.commit ()
            conn.close ()

            with open('/tmp/workfile', 'w') as f:
                f.write(content)
    
            self.respond (201, body="Danke")
        except:
            # Something went wrong.  At least send an error reply to
            # the client.
            self.respond (500)
            raise

    def respond (self, code, msg=None, headers=[], body=None):
        """
        Send a response to the client.  CODE is the http code, MSG is
        the HTTP status code.  HEADERS is a list of headers.  BODY is
        the body.
        """

        debug ("Respond: " + str (code) + ": " + str (msg))
        debug ("Headers: " + str (headers))
        debug ("Body (" + str (len (body or '')) + ") " + repr (body))

        if not msg:
            try:
                msg = httplib.responses [code]
            except KeyError:
                msg = ""
        self.send_response (code, msg)

        # Send headers.
        have_content_type = False
        have_content_length = False

        for key, value in headers:
            if key.lower () == 'content-type':
                have_content_type = True
            if key.lower () == 'content-length':
                have_content_length = True

            debug ('Writing ' + key + ': ' + str (value))
            self.send_header (key, str (value))

        if not have_content_type:
            debug ('Writing Content-Type: text/html; charset=utf-8')
            self.send_header ('Content-Type','text/html; charset=utf-8')
        if body and not have_content_length:
            debug ('Writing Content-Length: ' + str (len (body)))
            self.send_header ('Content-Length', str (len (body)))
        
        debug ('Writing Connection: close')
        self.send_header ("Connection", "close")
        self.end_headers ()

        if body:
            # Send the body.
            debug ("Body (" + str (len (body)) + "): " + repr (body))
            self.wfile.write (body)

        return


class uploader_server (SocketServer.ThreadingMixIn, BaseHTTPServer.HTTPServer):
    def __init__ (self, request_handler, binding=('0.0.0.0', 9321),
                  server_base=None):
        """
        ADDRESS is a tuple containing two elements, an IP address and
        a port.  By default, the server binds to port 9321 on all
        local IP addresses (0.0.0.0).
        """
        print "Listening on " + str (binding);
        print "Current directory: " + os.getcwd ();
        self.binding=binding
        self.server_base = server_base

        BaseHTTPServer.HTTPServer.__init__ (self, self.binding,
                                            request_handler)

        ctx = OpenSSL.SSL.Context (OpenSSL.SSL.SSLv23_METHOD)
        ctx.use_privatekey_file ('ssl-receiver.key')
        ctx.use_certificate_file ('ssl-receiver.cert')

        self.socket = OpenSSL.SSL.Connection \
            (ctx, socket.socket(self.address_family, self.socket_type))
        self.server_bind()
        self.server_activate()

    def run (self):
        self.serve_forever()

def main ():
    server = uploader_server (uploader_handler)
    server.run ()

if __name__ == "__main__":
    main ()
