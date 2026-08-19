#!/usr/bin/env python3

"""Static server for the browser build.

    tools/web/serve.py [port] [root]        # default 8111, build-web/Main

emrun is single-threaded and wedges when a script drives it, so
this exists instead. Three things it does that http.server does not:

- **Byte ranges.** The music is streamed through <audio> elements (docs/port-log.md §9.8), and a
  server that cannot answer `Range` makes that degrade quietly rather than
  fail: the element has to pull the whole file before it knows its duration, so
  it starts late, cannot seek, and the page's drift correction sits out
  until the download finishes because it waits on `duration`. Nothing errors.
  It just behaves worse than it will in production, which is the wrong thing
  for a server you are testing against.
- **The MIME types Emscripten needs.** .wasm has to arrive as application/wasm
  or streaming compilation refuses it.
- **No caching of the page and its scripts.** ivan.js is relinked constantly,
  and a cached copy is indistinguishable from a build that did not take. The
  audio and data files are left cacheable, which is the behaviour worth having
  while testing what gets fetched and when.
"""

import functools
import http.server
import os
import re
import shutil
import socketserver
import sys

http.server.SimpleHTTPRequestHandler.extensions_map['.wasm'] = 'application/wasm'
http.server.SimpleHTTPRequestHandler.extensions_map['.data'] = 'application/octet-stream'
http.server.SimpleHTTPRequestHandler.extensions_map['.ogg'] = 'audio/ogg'

RANGE = re.compile(r'^bytes=(\d*)-(\d*)$')

NO_CACHE = ('.html', '.js', '.json')


class Slice:
    """A file cut to a byte count, because copyfile() reads to EOF."""

    def __init__(self, handle, length):
        self.handle = handle
        self.left = length

    def read(self, size=-1):
        if self.left <= 0:
            return b''

        if size < 0 or size > self.left:
            size = self.left

        chunk = self.handle.read(size)
        self.left -= len(chunk)

        return chunk

    def close(self):
        self.handle.close()


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("HTTP %s\n" % (fmt % args))

    def send_head(self):
        path = self.translate_path(self.path)

        if os.path.isdir(path):
            return super().send_head()

        try:
            handle = open(path, 'rb')
        except OSError:
            self.send_error(404, "File not found")
            return None

        try:
            size = os.fstat(handle.fileno()).st_size
            start, end = 0, size - 1
            partial = False
            asked = self.headers.get('Range')
            match = RANGE.match(asked.strip()) if asked else None

            if match:
                first, last = match.group(1), match.group(2)

                if first:
                    start = int(first)

                    if last:
                        end = min(int(last), size - 1)
                elif last:
                    start = max(0, size - int(last))   # suffix: the last N bytes
                else:
                    match = None

            if match:
                if start > end or start >= size:
                    handle.close()
                    self.send_response(416)
                    self.send_header('Content-Range', 'bytes */%d' % size)
                    self.end_headers()
                    return None

                partial = True

            self.send_response(206 if partial else 200)
            self.send_header('Content-Type', self.guess_type(path))
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Content-Length', str(end - start + 1))

            if partial:
                self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))

            if os.path.splitext(path)[1].lower() in NO_CACHE:
                self.send_header('Cache-Control', 'no-store')

            self.end_headers()
            handle.seek(start)

            return Slice(handle, end - start + 1)

        except Exception:
            handle.close()
            raise

    def copyfile(self, source, outputfile):
        # Broken pipes are ordinary here: a media element abandons a range the
        # moment it has buffered enough, and the browser drops the connection.
        try:
            shutil.copyfileobj(source, outputfile)
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))

    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8111
    root = sys.argv[2] if len(sys.argv) > 2 else os.path.join(repo, "build-web", "Main")

    if not os.path.isdir(root):
        sys.exit("no such directory: %s\nBuild it first (CLAUDE.md)." % root)

    print("serving %s on http://localhost:%d/ivan.html" % (root, port))

    with Server(("0.0.0.0", port), functools.partial(Handler, directory=root)) as httpd:
        httpd.serve_forever()


if __name__ == "__main__":
    main()
