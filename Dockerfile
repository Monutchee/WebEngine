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

COPY www/          /www/
COPY nginx/nginx.conf /etc/nginx/nginx.conf
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 80 443

ENTRYPOINT ["/entrypoint.sh"]
