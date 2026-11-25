# mppm-server image (self-hosting example; see README.md). Not built by the project's CI.
#   docker build -t mppm-server .
# Stage 1: a static musl binary.
FROM rust:1.85-alpine AS build
RUN apk add --no-cache musl-dev
WORKDIR /src
COPY Cargo.toml Cargo.lock ./
COPY src ./src
RUN cargo build --release --locked && strip target/release/mppm-server

# Stage 2: the binary alone, as an unprivileged user.
FROM alpine:3.20
COPY --from=build /src/target/release/mppm-server /mppm-server
USER 65534:65534
# HTTP/WebSocket, builtin relay control port, builtin relay member ports.
EXPOSE 8080/tcp 3479/udp 41000-41999/udp
ENTRYPOINT ["/mppm-server", "--bind", "0.0.0.0:8080"]
