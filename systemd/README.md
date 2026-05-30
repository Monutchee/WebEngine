# Embedded deployment (systemd)

In this design the **backend owns nginx**: `webengine::NginxController` starts,
stops, reloads and re-ports nginx. So nginx must **not** also be managed by its
own distro service unit, or it would start twice (and as root, breaking the
same-user control model the reload/signal scheme depends on).

Install:

```bash
# 1. Stop the distro-managed nginx and prevent it from starting itself.
sudo systemctl disable --now nginx
sudo systemctl mask nginx          # belt-and-suspenders; beast-backend also has Conflicts=nginx.service

# 2. Let the unprivileged service user read/rewrite what the controller needs
#    (matches what the Dockerfile does for the container):
sudo chown -R www-data:www-data /etc/nginx/conf.d /etc/nginx/ssl
sudo chmod 640 /etc/nginx/ssl/server.key

# 3. Install and start the backend (which brings nginx up).
sudo cp beast-backend.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now beast-backend
```

Notes:
- nginx binds the ports in `conf.d/listen.conf` (8080/8443 by default) as `www-data`.
  To use ports < 1024 (e.g. 80/443), grant the binary the capability once:
  `sudo setcap 'cap_net_bind_service=+ep' /usr/sbin/nginx` — no root needed at runtime.
- On the embedded target nginx binds the NIC directly (no Docker NAT), so runtime
  port changes via `POST /api/admin/nginx/port` are immediately reachable. Leave
  `WEBENGINE_HTTP_PORT_MIN/MAX` unset to allow the full 1–65535 range.
- `beast-backend.service` cleans `/tmp/backend.sock` and `/tmp/nginx.pid` and
  recreates `/tmp/nginx` on every (re)start, so a crash never leaves stale state.
