# rpc-bench

`rpc-bench` is a clean-slate benchmark for a CRC32 hashing service. The
repository builds:

- an internal-only protocol/schema library
- `rpc-bench-server`
- `rpc-bench-bench`

The current implementation keeps one Cap'n Proto RPC contract and one KJ event
loop on the server for every transport. An internal `src/transport/` module
owns URI parsing, session setup, and transport runtime concerns shared by the
benchmark and server frontends.

## System Prerequisites

`rpc-bench` depends on a system-installed Cap'n Proto toolchain. Meson looks
for:

- `capnp` on `PATH`
- `capnpc-c++` on `PATH`
- pkg-config-visible `capnp`, `capnp-rpc`, `kj`, and `kj-async`

Common installation routes:

- `brew install capnp`
- `apt-get install capnproto`
- `pacman -S capnproto`

If Cap'n Proto is already installed but Meson cannot find it, expose the tools
through `PATH` and the pkg-config metadata through `PKG_CONFIG_PATH`.

## Build

Use the repository's local Meson native files. The default debug workflow is:

```sh
meson setup builddir --native-file native/clang22-debug.ini
meson compile -C builddir
```

## Service Contract

The service exposes one unary Cap'n Proto RPC:

- `hash(payload :Data) -> (crc32 :UInt32)`

Behavior:

- the CRC32 uses the standard IEEE polynomial
- the hash is computed over the payload bytes only
- zero-length payloads are valid
- the maximum payload size is 1 MiB

Oversized direct RPC calls fail as application/RPC errors rather than being
silently truncated.

## URI Surface

The public CLI is URI-first.

Supported listen or connect URIs:

- `tcp://HOST:PORT`
- `unix:///absolute/path`
- `pipe://socketpair`
- `shm://NAME`

Transport restrictions:

- `connect` supports `tcp://...`, `unix://...`, and `shm://NAME`
- `spawn-local` supports all four URI kinds
- `pipe://socketpair` remains `spawn-local` only because it requires inherited
  file descriptors
- `shm://NAME` remains a local transport, but it supports both `connect` and
  `spawn-local`

## `rpc-bench-server`

The server owns one KJ event loop for every transport and serves one bootstrap
capability over Cap'n Proto two-party RPC.

Example:

```sh
./builddir/src/server/rpc-bench-server \
  --listen-uri=tcp://127.0.0.1:7000
```

Unix-domain example:

```sh
./builddir/src/server/rpc-bench-server \
  --listen-uri=unix:///tmp/rpc-bench.sock
```

`pipe://socketpair` is intended for `spawn-local` mode, where the benchmark
process supplies the inherited descriptors for the child server automatically.

The benchmark CLI still keeps split flags: `--connect-uri` for `connect` mode
and `--listen-uri` for `spawn-local` mode.

When `--listen-uri=shm://NAME` is used, the server also requires
`--shm-slot-count=N` so it can size the shared-memory region up front.

Use `--quiet` to suppress the startup banner while keeping warnings and errors
on `stderr`.

## `rpc-bench-bench`

The benchmark keeps the two existing run modes:

- `spawn-local`: launch one local `rpc-bench-server` child process
- `connect`: benchmark one already running server

Each benchmark run uses:

- one OS thread per logical worker
- one RPC session per worker
- one outstanding request per worker
- one warmup phase and one measured phase
- per-request payload sizes sampled uniformly from an inclusive range

Example `spawn-local` run:

```sh
./builddir/src/bench/rpc-bench-bench \
  --mode=spawn-local \
  --listen-uri=tcp://127.0.0.1:7300 \
  --client-threads=4 \
  --message-size-min=128 \
  --message-size-max=256 \
  --warmup-seconds=0.5 \
  --measure-seconds=2.0 \
  --seed=1 \
  --json-output=report.json \
  --quiet-server
```

Example `connect` run:

```sh
./builddir/src/bench/rpc-bench-bench \
  --mode=connect \
  --connect-uri=tcp://127.0.0.1:7000 \
  --client-threads=4 \
  --message-size-min=128 \
  --message-size-max=256 \
  --warmup-seconds=0.5 \
  --measure-seconds=2.0 \
  --seed=1
```

Local transport examples:

- `--listen-uri=unix:///tmp/rpc-bench.sock`
- `--listen-uri=pipe://socketpair`
- `--listen-uri=shm://bench`
- `--connect-uri=shm://bench`

The default request-size range is `128-256` bytes when no size flags are
provided.

## Hybrid Shared Memory Transport

`shm://NAME` is a hybrid transport:

- the server creates the shared-memory region and sizes it with
  `--shm-slot-count=N`
- request and response message bytes move through shared-memory rings
- each benchmark run attaches through a derived Unix-socket sidecar rendezvous
  path
- each worker gets one logical slot, and spawn-local fills the slot count from
  `--client-threads`
- the sidecar carries attach, init, and wake notifications so the server stays
  on the KJ loop instead of a separate polling loop

## Reporting

The benchmark prints a terminal summary and can also emit JSON with
`--json-output=PATH`.

Both outputs keep the existing field names and include:

- mode and endpoint
- client thread count
- configured message-size range
- total requests and error count
- request bytes and response bytes
- request, response, and combined throughput
- RTT latency percentiles: `p50`, `p75`, `p90`, `p99`, `p99.9`

The reported byte counters intentionally preserve the earlier logical framing
accounting:

- `requestBytes` counts `4 + payload_size` for each successful measured RPC
- `responseBytes` counts `4` for each successful measured RPC

These are compatibility-oriented metrics, not literal Cap'n Proto wire bytes.

## Verification Scope

This milestone keeps verification manual and Meson-based:

- `meson compile -C builddir`
- one `spawn-local` and one `connect` smoke run for `tcp://...`
- one `spawn-local` and one `connect` smoke run for `unix://...`
- one `spawn-local` smoke run for `pipe://socketpair`
- one `spawn-local` and one `connect` smoke run for `shm://NAME`

This repository does not keep repo-managed automated tests or a coverage target
in this milestone.
