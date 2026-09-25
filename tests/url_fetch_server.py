#!/usr/bin/env python3
# Loopback HTTP server for url_fetch_test (desktop Linux fetcher, libcurl via
# dlopen). Serves a tiny glTF-binary payload plus the redirect / error shapes
# the fetcher must handle the way the WinHTTP one does. Stdlib only.
import http.server, sys, threading

GLB = b"glTF" + bytes(60)

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body=b"", ctype="application/octet-stream", headers=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path
        if p == "/a.glb":
            self._send(200, GLB)
        elif p == "/noext":  # extension from the magic bytes
            self._send(200, GLB)
        elif p == "/redir":
            self._send(302, headers=[("Location", "/redir-target.glb")])
        elif p == "/redir-target.glb":
            self._send(200, GLB)
        elif p == "/big.glb":
            self._send(200, GLB + bytes(4096))
        elif p == "/text":
            self._send(200, b"hello, not a model", "text/plain")
        else:
            self._send(404, b"nope", "text/plain")

srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
