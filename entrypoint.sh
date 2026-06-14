#!/bin/bash
set -euo pipefail

# The C++ backend is the single foreground process: it starts, controls and stops
# its reverse proxy itself via webengine::WebServerController. Which server it
# drives is chosen by WEBENGINE_WEBSERVER ("nginx" default, or "lighttpd"). The
# container runs as the www-data service user (docker-compose `user:`), and tini
# (compose `init: true`) reaps the daemon the backend spawns. On the embedded
# target, run the backend under its own systemd unit instead and drop the distro
# nginx/lighttpd service unit — the backend owns the server.

# Clean any stale runtime state from a previous run (both servers, so the image
# works whichever one is selected).
rm -f /tmp/backend.sock /tmp/nginx.pid /tmp/lighttpd.pid
mkdir -p /tmp/nginx /tmp/lighttpd

exec /usr/local/bin/backend
