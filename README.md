# rpc-bench

`rpc-bench` is a CRC32 RPC benchmark built on Cap'n Proto and KJ. The
repository builds:

- an internal-only protocol/schema library
- `rpc-bench-server`
- `rpc-bench-bench`

The current implementation keeps one Cap'n Proto RPC contract across all
transports. `tcp://...` and `unix://...` use one acceptor KJ event loop plus a
configurable number of serving worker event loops, while `pipe://socketpair`
remains single-loop. An internal application layer parses CLI URIs and run
modes, while the bench and server runtime layers talk only in terms of async
stream-backed RPC sessions.

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

## Test

Run the automated test suite with Meson:

```sh
meson test -C builddir --print-errorlogs
```

Run the benchmark smoke target through Meson's benchmark test mode:

```sh
meson test -C builddir --benchmark --print-errorlogs
```

Run the repository coverage workflow, which produces both a text summary and an
HTML report:

```sh
meson compile -C builddir coverage-report
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

Transport restrictions:

- `connect` supports `tcp://...` and `unix://...`
- `spawn-local` supports `tcp://...`, `unix://...`, and `pipe://socketpair`
- `pipe://socketpair` remains `spawn-local` only because it requires inherited
  file descriptors

## `rpc-bench-server`

The server serves one bootstrap capability over Cap'n Proto two-party RPC.

For `tcp://...` and `unix://...`, the runtime uses:

- one hidden acceptor thread with its own KJ event loop
- `--server-threads=N` serving worker threads, each with its own KJ event loop
- round-robin handoff of accepted network connections from the acceptor to the
  workers

For `pipe://socketpair`, the runtime stays on one KJ event loop and only
supports `--server-threads=1`.

Example:

```sh
./builddir/src/rpc-bench-server \
  --listen-uri=tcp://127.0.0.1:7000 \
  --server-threads=2
```

Unix-domain example:

```sh
./builddir/src/rpc-bench-server \
  --listen-uri=unix:///tmp/rpc-bench.sock \
  --server-threads=1
```

`pipe://socketpair` is intended for `spawn-local` mode, where the benchmark
process supplies the inherited descriptors for the child server automatically.

`--server-threads=N` counts serving worker threads only. For `tcp://...` and
`unix://...`, the server adds one hidden acceptor thread on top of those
workers. `pipe://socketpair` still rejects values greater than `1`.

Use `--quiet` to suppress the startup banner while keeping warnings and errors
on `stderr`.

## `rpc-bench-bench`

The benchmark keeps two run modes:

- `spawn-local`: launch one local `rpc-bench-server` child process
- `connect`: benchmark one already running server

Each benchmark run uses:

- `client_threads`: the number of benchmark event-loop threads
- `client_connections`: the total number of RPC connections
- connections distributed as evenly as possible across those threads
- zero or more connections per thread, so idle threads are allowed
- one outstanding request per connection at a time
- one warmup phase and one measured phase
- per-request payload sizes sampled uniformly from an inclusive range

Example `spawn-local` TCP run:

```sh
./builddir/src/rpc-bench-bench \
  --mode=spawn-local \
  --listen-uri=tcp://127.0.0.1:7300 \
  --server-threads=2 \
  --client-threads=2 \
  --client-connections=4 \
  --message-size-min=128 \
  --message-size-max=256 \
  --warmup-seconds=0.5 \
  --measure-seconds=2.0 \
  --seed=1 \
  --json-output=report.json \
  --quiet-server
```

Example `connect` Unix-domain run:

```sh
./builddir/src/rpc-bench-bench \
  --mode=connect \
  --connect-uri=unix:///tmp/rpc-bench.sock \
  --client-threads=2 \
  --client-connections=4 \
  --message-size-min=128 \
  --message-size-max=256 \
  --warmup-seconds=0.5 \
  --measure-seconds=2.0 \
  --seed=1
```

Example `spawn-local` pipe run:

```sh
./builddir/src/rpc-bench-bench \
  --mode=spawn-local \
  --listen-uri=pipe://socketpair \
  --server-threads=1 \
  --client-threads=2 \
  --client-connections=4 \
  --message-size-min=128 \
  --message-size-max=256 \
  --warmup-seconds=0.5 \
  --measure-seconds=2.0 \
  --seed=1 \
  --quiet-server
```

The default request-size range is `128-256` bytes when no size flags are
provided.

In `connect` mode the benchmark rejects `--server-threads`, because it cannot
configure an already-running remote server.

## Reporting

The benchmark prints a terminal summary and can also emit JSON with
`--json-output=PATH`.

Both outputs keep the existing field names and include:

- mode and endpoint
- client thread count
- client connection count
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
