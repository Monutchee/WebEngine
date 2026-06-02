#!/bin/bash
set -euo pipefail

# Mimic what the systemd units do, in the same order.
# On the embedded target, replace this container with the actual .service files.

cleanup() {
    echo "entrypoint: stopping services"
    kill "$NGINX_PID" "$BACKEND_PID" 2>/dev/null || true
    wait "$NGINX_PID" "$BACKEND_PID" 2>/dev/null || true
}
trap cleanup SIGTERM SIGINT

# ── beast-backend (mirrors ExecStartPre + ExecStart in beast-backend.service) ─
rm -f /tmp/backend.sock
su -s /bin/sh www-data -c '/usr/local/bin/backend' &
BACKEND_PID=$!

echo "entrypoint: waiting for /tmp/backend.sock"
until [ -S /tmp/backend.sock ]; do
    kill -0 "$BACKEND_PID" 2>/dev/null || { echo "entrypoint: backend died"; exit 1; }
    sleep 0.1
done
echo "entrypoint: backend ready"

# ── nginx (mirrors the nginx.service.d wait-for-backend.conf logic) ───────────
nginx -g 'daemon off;' &
NGINX_PID=$!
echo "entrypoint: nginx started (pid $NGINX_PID)"

# Exit if either process dies so Docker can restart the container.
wait -n "$BACKEND_PID" "$NGINX_PID"
echo "entrypoint: a process exited, shutting down"
cleanup
