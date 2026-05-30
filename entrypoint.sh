#!/bin/bash
set -euo pipefail

# The C++ backend is now the single foreground process: it starts, controls and
# stops nginx itself via webengine::NginxController. The container runs as the
# www-data service user (docker-compose `user:`), and tini (compose `init: true`)
# reaps the nginx daemon. On the embedded target, run the backend under its own
# systemd unit instead and drop nginx's service unit — the backend owns nginx.

# Clean any stale runtime state from a previous run.
rm -f /tmp/backend.sock /tmp/nginx.pid
mkdir -p /tmp/nginx

exec /usr/local/bin/backend
