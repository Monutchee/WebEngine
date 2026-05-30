# ── Stage 1: build the C++ backend ───────────────────────────────────────────
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libboost-dev \
        libboost-system-dev \
        libssl-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY backend/ .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    && cmake --build build --parallel "$(nproc)"

# ── Stage 2: slim runtime with nginx only ─────────────────────────────────────
# No systemd needed in Docker — the shell entrypoint starts both services.
# On the embedded target, use the systemd/ service files directly instead.
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        nginx \
        libssl3 \
        openssl \
    && rm -rf /var/lib/apt/lists/* \
    && rm -f /etc/nginx/sites-enabled/default

# Self-signed TLS certificate (PoC only).
# For the embedded target, replace server.crt / server.key with a real certificate
# issued by your CA — the nginx.conf SSL directives stay identical.
RUN mkdir -p /etc/nginx/ssl \
    && openssl req -x509 -nodes -days 3650 \
           -newkey rsa:2048 \
           -keyout /etc/nginx/ssl/server.key \
           -out    /etc/nginx/ssl/server.crt \
           -subj   "/CN=localhost" \
           -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

COPY --from=builder /src/build/backend /usr/local/bin/backend
RUN chmod 755 /usr/local/bin/backend

COPY www/             /www/
COPY nginx/nginx.conf /etc/nginx/nginx.conf
COPY nginx/conf.d/    /etc/nginx/conf.d/
COPY entrypoint.sh    /entrypoint.sh
RUN chmod +x /entrypoint.sh

# nginx runs as the unprivileged service user (www-data) so the same-user C++
# backend can control it. Give that user what it needs to read/rewrite:
#   • conf.d  — NginxController rewrites conf.d/listen.conf at runtime
#   • ssl     — the non-root master must be able to read the cert + key
#   • runtime dirs for the pidfile and temp paths (see nginx.conf)
RUN chown -R www-data:www-data /etc/nginx/conf.d /etc/nginx/ssl \
    && chmod 640 /etc/nginx/ssl/server.key \
    && mkdir -p /tmp/nginx \
    && chown www-data:www-data /tmp/nginx /var/lib/nginx 2>/dev/null || true

# High ports nginx binds as a non-root user (see conf.d/listen.conf). The 8080-8085
# range matches the runtime-changeable HTTP range published by docker-compose.
EXPOSE 8080-8085 8443

ENTRYPOINT ["/entrypoint.sh"]
