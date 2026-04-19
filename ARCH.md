# rpc-bench Architecture

## Overview

`rpc-bench` measures one simple remote service: a unary Cap'n Proto RPC whose
result is the CRC32 hash of a request payload. The observable system consists
of:

- an internal `protocol` library shared by both frontends
- `rpc-bench-server`, a single-endpoint hash server
- `rpc-bench-bench`, a benchmark driver that runs against one endpoint

The implementation is intentionally small and explicit. This version does not
define persistence, sharding, multi-endpoint runs, or multi-threaded server
execution.

## Service Semantics

### RPC method

The server exposes one internal Cap'n Proto interface:

- `hash(payload :Data) -> (crc32 :UInt32)`

### Hashing rules

- The CRC32 uses the standard IEEE polynomial.
- The hash is computed over the request payload bytes only.
- A zero-length payload is valid.
- The maximum permitted payload length is 1 MiB.

### Invalid input handling

- If a benchmark configuration requests payloads above 1 MiB, the CLI rejects
  the run before it starts.
- If a direct RPC caller submits a payload above 1 MiB, the server rejects the
  call as an application/RPC failure.
- If a connection breaks during connect, request, or response handling, the
  affected client thread fails without affecting other benchmark threads.

## Server Semantics

### Runtime model

- One process owns one KJ async event loop.
- One process listens on one endpoint.
- The server is single-threaded at the request-processing level in this
  milestone.
- The server uses a single bootstrap capability and Cap'n Proto two-party RPC
  to service accepted connections.

### Lifecycle

- The server binds before it starts waiting for shutdown signals.
- Unless `--quiet` is set, it prints one startup banner with the bound
  endpoint.
- `SIGINT` and `SIGTERM` are captured through KJ's Unix event port and cancel
  the listener from within the event loop.
- After shutdown is requested, the process exits the event loop and terminates.

## Benchmark Semantics

### Run modes

- `connect` benchmarks one already running endpoint.
- `spawn-local` starts one local child `rpc-bench-server`, waits for the
  endpoint to answer a probe RPC, runs the workload, and then stops the child.

Every benchmark invocation targets exactly one endpoint.

### Client concurrency model

- `client_threads` means one OS thread per logical benchmark worker.
- Each worker thread owns one KJ event loop.
- Each worker thread owns one RPC connection for the entire run.
- Each worker thread keeps exactly one request outstanding at a time.

This is a closed-loop benchmark. A worker sends the next RPC only after the
previous reply arrives.

### Workload contents

- Request payload sizes are sampled uniformly from an inclusive size range.
- The default range is `128-256` bytes.
- Payload contents are generated from a deterministic PRNG stream derived from
  the configured seed and thread index.
- Each invocation has one warmup phase and one measured phase.
- Warmup traffic is not included in the reported statistics.

## Reporting Semantics

### Counted traffic

The report intentionally preserves the legacy logical byte accounting that
predates the Cap'n Proto transport change.

- `requestBytes` counts `4 + payload_size` for each successful measured RPC.
- `responseBytes` counts `4` for each successful measured RPC.

These counters are compatibility-oriented metrics, not literal Cap'n Proto wire
bytes.

### Latency

- Latency is round-trip time from the start of request submission until the RPC
  reply has been received completely.
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

- `src/protocol/`: internal schema generation, payload-limit validation,
  endpoint parsing, and CRC32 implementation
- `src/server/`: CLI parsing plus the KJ listener lifecycle and bootstrap
  capability
- `src/bench/`: CLI parsing, child-process management for `spawn-local`,
  workload generation, RPC client loops, metric aggregation, and report
  rendering

The protocol layer is internal-only. This repository does not expose or install
a public SDK surface in this version.

## Design Decisions And Trade-offs

### Why Cap'n Proto RPC over KJ

The implementation goal for this milestone is to replace the Asio-based
transport/runtime stack with Cap'n Proto RPC while keeping the CLI and report
surface stable. Cap'n Proto's generated interfaces and KJ event loop provide an
explicit RPC stack without introducing a separate application-specific protocol
parser.

### Why a system-installed Cap'n Proto toolchain

This repository carries no bundled dependency-acquisition path. The build must
use the operator's installed `capnp`, `capnpc-c++`, and pkg-config-visible
libraries so configuration failures point directly at the local toolchain state
instead of silently downloading another copy.

### Why keep one outstanding request per connection

One in-flight request per connection keeps RTT semantics unambiguous and matches
the benchmark's user-visible concurrency model. The trade-off is that this
version does not explore pipelining or open-loop traffic generation.

### Why preserve legacy byte counters

The transport changed, but the terminal and JSON report fields remain stable so
existing users can compare runs without a report-schema migration. The trade-off
is that `requestBytes` and `responseBytes` are logical compatibility metrics
rather than literal on-the-wire Cap'n Proto sizes.

## Non-goals For This Milestone

- multi-threaded server execution
- multiple endpoints in one benchmark invocation
- persistence or recovery semantics
- backward compatibility with the deleted raw framed TCP transport
- a public reusable client or server SDK
- repo-managed automated tests or coverage-report plumbing
