#!/usr/bin/env python3
"""Minimal CORS-enabled static server for the INFLATE deployment outputs.

Serves http://localhost:8185/outputs/... from the deployment directory, with
the headers the WASM viewer needs (it is served under COEP require-corp by
emrun, so cross-origin fetches must pass a CORS check).

No root required -- an alternative to the nginx block. Stop with Ctrl-C or by
killing the process.
"""
import functools, http.server, socketserver

ROOT = "/home/arash/Projects/DrywellDT/deployments/Reservoir_truth"
PORT = 8094

class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cross-Origin-Resource-Policy", "cross-origin")
        self.send_header("Cache-Control", "no-cache, must-revalidate")
        super().end_headers()
    def log_message(self, *a):        # keep the terminal quiet
        pass

socketserver.TCPServer.allow_reuse_address = True
with socketserver.ThreadingTCPServer(("127.0.0.1", PORT),
                                     functools.partial(H, directory=ROOT)) as httpd:
    print(f"serving {ROOT} on http://localhost:{PORT}")
    httpd.serve_forever()
