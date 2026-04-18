# rpc-bench Architecture

## Overview

`rpc-bench` measures one simple remote service: framed TCP requests whose
responses are the CRC32 hash of the request payload. The observable system
consists of:

- an internal `protocol` library shared by both frontends
- `rpc-bench-server`, a single-endpoint hash server
- `rpc-bench-bench`, a benchmark driver that runs against one endpoint

The implementation is intentionally small and explicit. This version does not
define persistence, sharding, multi-endpoint runs, or a multi-threaded server.

## Wire Semantics

### Request frame

- Each request begins with a 4-byte unsigned big-endian payload length.
- The length is followed immediately by that many payload bytes.
- A zero-length payload is valid.
- The maximum permitted payload length is 1 MiB.

### Response frame

- Each successful response is one 4-byte unsigned big-endian CRC32 value.
- The CRC32 uses the standard IEEE polynomial and is computed over the request
  payload bytes only.

### Invalid input handling

- If a client advertises a payload length above 1 MiB, the server closes the
  connection.
- If a connection breaks during a frame read or write, the server drops that
  connection without affecting other clients.

## Server Semantics

### Runtime model

- One process owns one `asio::io_context`.
- One process listens on one TCP endpoint.
- The server is single-threaded and async in this milestone.
- Each accepted connection is handled by its own coroutine.
- Each connection is strictly request/response serial: the next request is not
  read until the current reply has been written.

### Lifecycle

- The server starts listening before entering the main event loop.
- Unless `--quiet` is set, it prints one startup banner with the bound
  endpoint.
- `SIGINT` and `SIGTERM` trigger graceful shutdown by stopping accepts and
  letting the process exit the event loop.

## Benchmark Semantics

### Run modes

- `connect` benchmarks one already running endpoint.
- `spawn-local` starts one local child `rpc-bench-server`, waits for the
  endpoint to become reachable, runs the workload, and then stops the child.

Every benchmark invocation targets exactly one endpoint.

### Client concurrency model

- `client_threads` is one OS thread per connection.
- Each thread owns one TCP socket for the entire run.
- Each thread keeps exactly one request outstanding at a time.

This is a closed-loop benchmark. A client thread sends the next request only
after the previous response arrives.

### Workload contents

- Request payload sizes are sampled uniformly from an inclusive size range.
- The default range is `128-256` bytes.
- Payload contents are generated from a deterministic PRNG stream derived from
  the configured seed and thread index.
- Each invocation has one warmup phase and one measured phase.
- Warmup traffic is not included in the reported statistics.

## Reporting Semantics

### Counted traffic

- `requestBytes` counts wire bytes sent during the measured phase:
  4-byte length prefixes plus payload bytes.
- `responseBytes` counts wire bytes received during the measured phase:
  one 4-byte CRC32 reply per successful request.

### Latency

- Latency is round-trip time from the start of the request send until the full
  4-byte response has been received.
- Reported percentiles are `p50`, `p75`, `p90`, `p99`, and `p99.9`.
- Percentiles are computed from successful measured requests only.

### Throughput

- Request throughput is `requestBytes / measuredSeconds`.
- Response throughput is `responseBytes / measuredSeconds`.
- Combined throughput is `(requestBytes + responseBytes) / measuredSeconds`.
- Request-rate throughput is `totalRequests / measuredSeconds`.

The benchmark emits both a terminal summary and an optional JSON document with
the same core fields.

## Implementation Structure

- `src/protocol/`: internal parsing helpers, size-range validation, framing
  helpers, and CRC32 implementation
- `src/server/`: CLI parsing, async listener setup, per-connection coroutines,
  and process lifecycle
- `src/bench/`: CLI parsing, child-process management for `spawn-local`,
  workload generation, metric aggregation, and report rendering

The protocol layer is internal-only. This repository does not expose or install
a public SDK surface in this version.

## Design Decisions And Trade-offs

### Why raw framed TCP

The benchmark is focused on transport and request/response timing for a tiny
hashing service. Raw framed TCP keeps the wire contract small and auditable and
avoids hiding benchmark costs behind a richer RPC stack.

### Why standalone Asio

Standalone Asio provides a compact async runtime that works well with C++23
coroutines and fits the single-threaded event-loop design without introducing a
larger framework surface.

### Why one outstanding request per connection

One in-flight request per connection keeps RTT semantics unambiguous and
matches the user-requested client model. The trade-off is that this version
does not explore pipelining or open-loop traffic generation.

## Non-goals For This Milestone

- multi-threaded server execution
- multiple endpoints in one benchmark invocation
- persistence or recovery semantics
- compatibility with the deleted Cap'n Proto key-value benchmark
- automated tests, coverage tracking, or benchmark sweeps beyond one run per
  invocation
