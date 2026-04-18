# rpc-bench

`rpc-bench` is a clean-slate TCP benchmark for a CRC32 hashing service. The
repository builds:

- an internal-only `protocol` library
- `rpc-bench-server`
- `rpc-bench-bench`

Clients send arbitrary byte payloads to the server. The server replies with the
CRC32 hash of each payload as one fixed 32-bit value. The benchmark focuses on
single-endpoint runs, single-threaded async server execution, and one
connection-owning client thread per benchmark thread.

## Build

Use the repository's local Meson native files. The default debug workflow is:

```sh
meson setup builddir --native-file native/clang22-debug.ini
meson compile -C builddir
```

The first configure downloads standalone Asio from WrapDB. The retained
`subprojects/capnproto.wrap` file stays in the tree, but this benchmark does
not use Cap'n Proto in this version.

## Protocol

The wire protocol is raw framed TCP:

- request: 4-byte big-endian payload length followed by payload bytes
- response: 4-byte big-endian CRC32 value
- maximum request payload: 1 MiB

If a client advertises a payload larger than 1 MiB, the server closes the
connection instead of attempting the read.

## `rpc-bench-server`

The server owns one `asio::io_context`, one TCP acceptor, and one coroutine per
connection. It is intentionally single-threaded in this milestone.

Example:

```sh
./builddir/src/server/rpc-bench-server \
  --listen-host=127.0.0.1 \
  --port=7000
```

Use `--quiet` to suppress the startup banner while keeping warnings and errors
on `stderr`.

## `rpc-bench-bench`

The benchmark supports two modes:

- `spawn-local`: launch one local `rpc-bench-server` child process
- `connect`: benchmark one already running endpoint

Each benchmark run uses:

- one thread per connection
- one TCP socket per thread
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

Both outputs include:

- mode and endpoint
- client thread count
- configured message-size range
- total requests and error count
- request bytes and response bytes
- request, response, and combined throughput
- RTT latency percentiles: `p50`, `p75`, `p90`, `p99`, `p99.9`

Latency is measured from the start of the request send until the full 4-byte
response is received.

## Verification Scope

This milestone intentionally does not reintroduce automated tests or coverage
reporting. Verification is limited to building the project and running manual
`spawn-local` and `connect` smoke benchmarks.
