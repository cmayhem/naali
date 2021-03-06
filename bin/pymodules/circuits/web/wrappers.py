# Module:   wrappers
# Date:     13th September 2007
# Author:   James Mills, prologic at shortcircuit dot net dot au

"""Request/Response Wrappers

This module implements the Request and Response objects.
"""


import os
from cStringIO import StringIO
from time import strftime, time
from Cookie import SimpleCookie
from types import FileType, ListType

from utils import url
from headers import Headers
from constants import SERVER_VERSION
from circuits.net.sockets import BUFSIZE

def file_generator(input, chunkSize=BUFSIZE):
    chunk = input.read(chunkSize)
    while chunk:
        yield chunk
        chunk = input.read(chunkSize)
    input.close()

class Host(object):
    """An internet address.

    name should be the client's host name. If not available (because no DNS
    lookup is performed), the IP address should be used instead.
    """

    ip = "0.0.0.0"
    port = 80
    name = "unknown.tld"

    def __init__(self, ip, port, name=None):
        self.ip = ip
        self.port = port
        if name is None:
            name = ip
            self.name = name

    def __repr__(self):
        return "Host(%r, %r, %r)" % (self.ip, self.port, self.name)

class Request(object):
    """Creates a new Request object to hold information about a request.
    
    :param sock: The socket object of the request.
    :type  sock: socket.socket

    :param method: The requsted method.
    :type  method: str

    :param scheme: The requsted scheme.
    :type  scheme: str

    :param path: The requsted path.
    :type  path: str

    :param protocol: The requsted protocol.
    :type  protocol: str

    :param qs: The query string of the request.
    :type  qs: str
    """

    server = None
    """@cvar: A reference to the underlying server"""

    scheme = "http"
    protocol = (1, 1)
    server_protocol = (1, 1)
    host = ""
    local = Host("127.0.0.1", 80)
    remote = Host("127.0.0.1", 1111)

    xhr = False

    index = None
    script_name = ""

    login = None
    handled = False

    def __init__(self, sock, method, scheme, path, protocol, qs):
        "initializes x; see x.__class__.__doc__ for signature"

        self.sock = sock
        self.method = method
        self.scheme = scheme or Request.scheme
        self.path = path
        self.protocol = protocol
        self.qs = qs
        self.cookie = SimpleCookie()

        self._headers = None

        if sock:
            name = sock.getpeername()
            if name:
                self.remote = Host(*name)
            else:
                name = sock.getsockname()
                self.remote = Host(name, "", name)

        self.body = StringIO()

    def _getHeaders(self):
        return self._headers

    def _setHeaders(self, headers):
        self._headers = headers

        if "Cookie" in self.headers:
            self.cookie.load(self.headers["Cookie"])

        host = self.headers.get("Host", None)
        if not host:
            host = self.local.name or self.local.ip
        self.base = "%s://%s" % (self.scheme, host)

        self.xhr = self.headers.get("X-Requested-With", "").lower() == \
                "xmlhttprequest"

    headers = property(_getHeaders, _setHeaders)

    def __repr__(self):
        protocol = "HTTP/%d.%d" % self.protocol
        return "<Request %s %s %s>" % (self.method, self.path, protocol)

    def url(self):
        return url(self)

class Body(object):
    """Response Body"""
    
    def __get__(self, response, cls=None):
        if response is None:
            return self
        else:
            return response._body
    
    def __set__(self, response, value):
        if isinstance(value, basestring):
            if value:
                value = [value]
            else:
                value = []
        elif isinstance(value, FileType):
            response.stream = True
            value = file_generator(value)
        elif value is None:
            value = []
        response._body = value

class Response(object):
    """Response(sock, request) -> new Response object

    A Response object that holds the response to
    send back to the client. This ensure that the correct data
    is sent in the correct order.
    """

    chunked = False
    body = Body()

    def __init__(self, sock, request):
        "initializes x; see x.__class__.__doc__ for signature"

        self.sock = sock
        self.request = request
        self.clear()

    def __repr__(self):
        return "<Response %s %s (%d)>" % (
                self.status,
                self.headers["Content-Type"],
                (len(self.body) if type(self.body) == str else 0))
    
    def __str__(self):
        self.prepare()
        protocol = self.protocol
        status = self.status
        headers = self.headers
        return "%s %s\r\n%s" % (protocol, status, headers)

    def clear(self):
        self.done = False
        self.close = False
        
        if self.request.server:
            server_version = self.request.server.version
        else:
            server_version = SERVER_VERSION

        self.headers = Headers([
            ("Date", strftime("%a, %d %b %Y %H:%M:%S %Z")),
            ("X-Powered-By", server_version)])

        if self.request.server is not None:
            self.headers.add_header("Server", server_version)

        self.cookie = self.request.cookie

        self.stream = False
        self._body = []
        self.time = time()
        self.status = "200 OK"
        self.protocol = "HTTP/%d.%d" % self.request.server_protocol

    def prepare(self):
        if self.body and type(self.body) is ListType:
            if unicode in map(type, self.body):
                cLength = sum(map(lambda s: len(s.encode("utf-8")), self.body))
            else:
                cLength = sum(map(len, self.body))

            self.headers.setdefault("Content-Type", "text/html")
            self.headers["Content-Length"] = str(cLength)

        if self.stream:
            self.headers.setdefault("Content-Type", "application/octet-stream")

        for k, v in self.cookie.iteritems():
            self.headers.add_header("Set-Cookie", v.OutputString())

        status = int(self.status.split(" ", 1)[0])

        if status == 413:
            self.close = True
        elif "Content-Length" not in self.headers:
            if status < 200 or status in (204, 205, 304):
                pass
            else:
                if self.protocol == "HTTP/1.1" \
                        and self.request.method != "HEAD" \
                        and self.request.server is not None:
                    self.chunked = True
                    self.headers.add_header("Transfer-Encoding", "chunked")
                else:
                    self.close = True

        if self.request.server is not None and "Connection" not in self.headers:
            if self.protocol == "HTTP/1.1":
                if self.close:
                    self.headers.add_header("Connection", "close")
            else:
                if not self.close:
                    self.headers.add_header("Connection", "Keep-Alive")

        if self.headers.get("Transfer-Encoding", "") == "chunked":
            self.chunked = True
