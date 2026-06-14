# Embedded deployment (systemd)

In this design the **backend owns its reverse proxy**:
`webengine::WebServerController` starts, stops, reloads and re-ports it. The proxy
is **nginx** by default, or **lighttpd** (the lighter choice for older platforms)
when `WEBENGINE_WEBSERVER=lighttpd`. Either way that server must **not** also be
managed by its own distro service unit, or it would start twice (and as root,
breaking the same-user control model the reload/signal scheme depends on).

Install (nginx shown; for lighttpd, swap the paths and unit name as noted):

```bash
# 1. Stop the distro-managed server and prevent it from starting itself.
sudo systemctl disable --now nginx
sudo systemctl mask nginx          # belt-and-suspenders; beast-backend also has Conflicts=nginx.service
#   lighttpd:  sudo systemctl disable --now lighttpd && sudo systemctl mask lighttpd
#              (and add `Conflicts=lighttpd.service` to beast-backend.service)

# 2. Let the unprivileged service user read/rewrite what the controller needs
#    (matches what the Dockerfile does for the container):
sudo chown -R www-data:www-data /etc/nginx/conf.d /etc/nginx/ssl
sudo chmod 640 /etc/nginx/ssl/server.key
#   lighttpd:  sudo chown -R www-data:www-data /etc/lighttpd/conf.d /etc/lighttpd/ssl
#              sudo chmod 640 /etc/lighttpd/ssl/server.pem

# 3. Pick the server (default nginx). For lighttpd, uncomment the
#    Environment=WEBENGINE_WEBSERVER=lighttpd line in beast-backend.service.

# 4. Install and start the backend (which brings the server up).
sudo cp beast-backend.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now beast-backend
```

Notes:
- The server binds the ports in `conf.d/listen.conf` (8080/8443 by default) as
  `www-data`. To use ports < 1024 (e.g. 80/443), grant the binary the capability
  once: `sudo setcap 'cap_net_bind_service=+ep' /usr/sbin/nginx` (or
  `/usr/sbin/lighttpd`) — no root needed at runtime.
- On the embedded target the server binds the NIC directly (no Docker NAT), so
  runtime port changes via `POST /api/admin/server/port` are immediately reachable.
  Leave `WEBENGINE_HTTP_PORT_MIN/MAX` unset to allow the full 1–65535 range.
- `beast-backend.service` cleans `/tmp/backend.sock` and both servers' pidfiles and
  recreates `/tmp/nginx` and `/tmp/lighttpd` on every (re)start, so a crash never
  leaves stale state.
- lighttpd has no graceful config reload on these platforms: `reload()` and
  `set_listen_port()` restart it (briefly). It also has no `auth_request`, so it
  proxies `/protected/` to the backend, which gates + serves those files. See the
  main README for the full list of nginx-vs-lighttpd differences.
