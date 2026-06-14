# ── Stage 1: build the C++ backend ───────────────────────────────────────────
# Built on the official GCC 13 image rather than debian:bookworm-slim because the
# JSON layer (Glaze) needs a C++23 compiler
# and supports GCC 13+, while bookworm only ships GCC 12. This image is itself
# Debian bookworm based, so the backend's dynamically-linked glibc matches the
# bookworm runtime stage (a newer-glibc builder would fail at runtime).
FROM gcc:13-bookworm AS builder

ARG GLAZE_VERSION=7.7.1

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        libboost-dev \
        libboost-system-dev \
        libssl-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install Glaze's headers and CMake package so the backend can use find_package().
RUN git clone --depth 1 --branch "v${GLAZE_VERSION}" \
        https://github.com/stephenberry/glaze.git /tmp/glaze \
    && cmake -S /tmp/glaze -B /tmp/glaze-build \
        -Dglaze_BUILD_EXAMPLES=OFF \
        -Dglaze_DEVELOPER_MODE=OFF \
    && cmake --install /tmp/glaze-build --prefix /usr/local \
    && rm -rf /tmp/glaze /tmp/glaze-build

WORKDIR /src
COPY backend/ .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    && cmake --build build --parallel "$(nproc)"

# ── Stage 2: slim runtime with both reverse proxies ───────────────────────────
# Installs BOTH nginx and lighttpd; the backend drives exactly ONE of them per
# run, chosen by WEBENGINE_WEBSERVER (see entrypoint.sh / docker-compose). No
# systemd needed in Docker — the shell entrypoint starts the backend, which owns
# its server. On the embedded target, use the systemd/ service files instead.
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        nginx \
        lighttpd \
        lighttpd-mod-openssl \
        libssl3 \
        openssl \
    && rm -rf /var/lib/apt/lists/* \
    && rm -f /etc/nginx/sites-enabled/default

# Self-signed TLS certificate (PoC only), shared by both servers.
# For the embedded target, replace server.crt / server.key with a real certificate
# issued by your CA — the server SSL directives stay identical. nginx reads the
# separate crt/key; lighttpd reads a combined PEM (cert followed by key).
RUN mkdir -p /etc/nginx/ssl /etc/lighttpd/ssl \
    && openssl req -x509 -nodes -days 3650 \
           -newkey rsa:2048 \
           -keyout /etc/nginx/ssl/server.key \
           -out    /etc/nginx/ssl/server.crt \
           -subj   "/CN=localhost" \
           -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    && cat /etc/nginx/ssl/server.crt /etc/nginx/ssl/server.key \
         > /etc/lighttpd/ssl/server.pem

COPY --from=builder /src/build/backend /usr/local/bin/backend
RUN chmod 755 /usr/local/bin/backend

COPY www/                 /www/
COPY nginx/nginx.conf     /etc/nginx/nginx.conf
COPY nginx/conf.d/        /etc/nginx/conf.d/
# Overwrite Debian's stock lighttpd.conf with ours (which does not pull in
# conf-enabled/*, so the distro defaults stay inert).
COPY lighttpd/lighttpd.conf /etc/lighttpd/lighttpd.conf
COPY lighttpd/conf.d/       /etc/lighttpd/conf.d/
COPY entrypoint.sh        /entrypoint.sh
RUN chmod +x /entrypoint.sh

# Each server runs as the unprivileged service user (www-data) so the same-user
# C++ backend can control it. Give that user what it needs to read/rewrite:
#   • conf.d  — the controller rewrites conf.d/listen.conf at runtime
#   • ssl     — the non-root master must be able to read the cert/key/pem
#   • runtime dirs for the pidfile and temp paths
RUN chown -R www-data:www-data \
        /etc/nginx/conf.d /etc/nginx/ssl \
        /etc/lighttpd/conf.d /etc/lighttpd/ssl \
    && chmod 640 /etc/nginx/ssl/server.key /etc/lighttpd/ssl/server.pem \
    && mkdir -p /tmp/nginx /tmp/lighttpd \
    && chown www-data:www-data /tmp/nginx /tmp/lighttpd /var/lib/nginx 2>/dev/null || true

# High ports each server binds as a non-root user (see conf.d/listen.conf). The
# nginx service publishes 8080-8085, the lighttpd service 9080-9085 — different
# ranges so both can run side-by-side (see docker-compose.yml).
EXPOSE 8080-8085 8443 9080-9085 9443

ENTRYPOINT ["/entrypoint.sh"]
