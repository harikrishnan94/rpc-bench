# rpc-bench

`rpc-bench` is a clean-slate benchmark for a CRC32 hashing service. The
repository builds:

- an internal-only protocol/schema library
- `rpc-bench-server`
- `rpc-bench-bench`

The current implementation keeps the existing CLI, lifecycle, and report shape
while moving the transport from raw framed TCP to Cap'n Proto RPC on top of
KJ async I/O.

## System Prerequisites

`rpc-bench` depends on a system-installed Cap'n Proto toolchain. Meson will
look for:

- `capnp` on `PATH`
- `capnpc-c++` on `PATH`
- pkg-config-visible `capnp`, `capnp-rpc`, `kj`, and `kj-async`

Common installation routes:

- `brew install capnp`
- `apt-get install capnproto`
- `pacman -S capnproto`

If Cap'n Proto is already installed but Meson cannot find it, expose the
compiler tools through `PATH` and the pkg-config metadata through
`PKG_CONFIG_PATH`.

## Build

Use the repository's local Meson native files. The default debug workflow is:

```sh
meson setup builddir --native-file native/clang22-debug.ini
meson compile -C builddir
```

This repository ships no dependency-acquisition metadata or Meson subproject
fallback for third-party libraries. Configuration stops with a friendly hint if
the system toolchain is missing.

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

## `rpc-bench-server`

The server owns one KJ event loop, listens on one endpoint, and serves the
hashing capability over Cap'n Proto's two-party RPC transport.

Example:

```sh
./builddir/src/server/rpc-bench-server \
  --listen-host=127.0.0.1 \
  --port=7000
```

Use `--quiet` to suppress the startup banner while keeping warnings and errors
on `stderr`.

## `rpc-bench-bench`

The benchmark keeps the existing two modes:

- `spawn-local`: launch one local `rpc-bench-server` child process
- `connect`: benchmark one already running endpoint

Each benchmark run uses:

- one OS thread per connection
- one RPC connection per thread
- one outstanding request per connection
- one warmup phase and one measured phase
- per-request payload sizes sampled uniformly from an inclusive range

Example `spawn-local` run:

```sh
./builddir/src/bench/rpc-bench-bench \
  --mode=spawn-local \
  --listen-host=127.0.0.1 \
  --server-port=7300 \
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
  --endpoint=127.0.0.1:7000 \
  --client-threads=4 \
  --message-size-min=128 \
  --message-size-max=256 \
  --warmup-seconds=0.5 \
  --measure-seconds=2.0 \
  --seed=1
```

The default request-size range is `128-256` bytes when no size flags are
provided.

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

This milestone does not keep repo-managed automated tests or a coverage target.
Verification is limited to configuring and building the project plus manual
`spawn-local` and `connect` smoke runs.
