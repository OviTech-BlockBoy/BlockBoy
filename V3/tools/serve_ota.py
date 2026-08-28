#!/usr/bin/env python3
"""Serves build/ota/ over HTTP, to test OTA without publishing anything.

Usage:

    python tools/publish_update.py 3.0.1 --base-url http://192.168.1.50:8000
    python tools/serve_ota.py

The IP you pass to --base-url must be this PC's address on your own network;
this script prints which addresses it sees. 'localhost' does not work -- the
device must be able to reach it.

On the test device's SD card, put this in /BlockBoy/config/ota.json:

    { "ManifestURL": "http://192.168.1.50:8000/v3-n16r8.json" }

As long as that override is present the device shows '[TEST]' in the update
dialog. Remove it before you hand the device over.

Note: this uses plain HTTP, so the HTTPS route to GitHub is not tested here.
Do that check separately before going live.
"""

import argparse
import functools
import http.server
import os
import socket
import sys

DEFAULT_DIR = os.path.join("build", "ota")
DEFAULT_PORT = 8000


def local_addresses():
    """All IPv4 addresses this PC might be reachable on."""
    found = []
    try:
        # Connect to an address outside the network without actually sending;
        # this makes the socket reveal the IP of the interface it would use.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("8.8.8.8", 80))
        found.append(probe.getsockname()[0])
        probe.close()
    except OSError:
        pass

    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addr = info[4][0]
            if addr not in found and not addr.startswith("127."):
                found.append(addr)
    except socket.gaierror:
        pass

    return found


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # By default the handler logs to stderr with a timestamp; a compact
        # line per request is more useful here to follow an update.
        sys.stdout.write("  %s\n" % (fmt % args))
        sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser(description="Serve OTA files for local testing")
    parser.add_argument("--dir", default=DEFAULT_DIR, help="Directory to serve")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    if not os.path.isdir(args.dir):
        sys.exit(
            "'%s' does not exist.\n"
            "First run: python tools/publish_update.py <version> --base-url http://<your-ip>:%d"
            % (args.dir, args.port)
        )

    entries = sorted(os.listdir(args.dir))
    if not entries:
        sys.exit("'%s' is empty." % args.dir)

    print("\nServing %s on port %d\n" % (os.path.abspath(args.dir), args.port))

    addresses = local_addresses()
    if addresses:
        print("Reachable via:")
        for addr in addresses:
            print("  http://%s:%d/" % (addr, args.port))
    else:
        print("Could not determine a network address; look up this PC's IP manually.")

    manifests = [e for e in entries if e.endswith(".json")]
    if manifests and addresses:
        print("\nPut this on the SD card in /BlockBoy/config/ota.json:")
        print('  { "ManifestURL": "http://%s:%d/%s" }' % (addresses[0], args.port, manifests[0]))

    print("\nFiles: %s" % ", ".join(entries))
    print("\nCtrl-C to stop.\n")

    handler = functools.partial(QuietHandler, directory=args.dir)
    server = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), handler)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
        server.server_close()


if __name__ == "__main__":
    main()
