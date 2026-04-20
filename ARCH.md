# rpc-bench Architecture

## Overview

`rpc-bench` measures one simple remote service: a unary RPC whose result is the
CRC32 hash of a request payload. The observable system consists of:

- an internal `protocol` library shared by both frontends
- `rpc-bench-server`, a single-listen-target hash server
- `rpc-bench-bench`, a benchmark driver that runs against one target

The implementation is intentionally small and explicit. This version does not
define persistence, sharding, multi-target runs, or multi-threaded server
execution.

An internal `src/transport/` module owns URI parsing, session setup, and the
transport runtime policy shared by the benchmark and server frontends.

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
  call as an application or RPC failure.
- If a session breaks during connect, request, or response handling, the
  affected worker fails without affecting other workers.

## Transport Semantics

### Public URI surface

The user-visible transport identifier is always a URI.

Supported URI kinds:

- `tcp://HOST:PORT`
- `unix:///absolute/path`
- `pipe://socketpair`
- `shm://NAME`

### Transport boundary

- `tcp://...` uses KJ `NetworkAddress` and `ConnectionReceiver`.
- `unix://...` uses KJ `NetworkAddress` and `ConnectionReceiver`.
- `pipe://socketpair` uses one pre-connected stream per worker.
- `shm://NAME` uses a server-owned shared-memory region for request and
  response rings plus a derived Unix-socket sidecar rendezvous path for attach,
  init, and wake notifications.

### Mode restrictions

- `connect` is defined for `tcp://...`, `unix://...`, and `shm://NAME`.
- `spawn-local` is defined for all supported URI kinds.
- `pipe://socketpair` remains `spawn-local` only because it depends on
  inherited pre-connected file descriptors.
- `shm://NAME` is local-only in transport implementation, but it supports both
  `connect` and `spawn-local`.

## Server Semantics

### Runtime model

- One server process owns one KJ async event loop.
- The same KJ event loop invariant holds for every transport.
- Every accepted or pre-created session is serviced through one
  `TwoPartyVatNetwork` plus one `RpcSystem`.
- The server is single-threaded at the request-processing level in this
  milestone.
- The server exports one bootstrap capability that implements the hash RPC.

### Transport-specific session setup

- For `tcp://...` and `unix://...`, the server resolves the URI through KJ,
  binds one listener, and accepts streams from that listener.
- For `pipe://socketpair`, the benchmark parent creates one socketpair per
  worker before spawning the child server. The child wraps the inherited server
  ends into KJ streams and serves them as pre-connected RPC sessions.
- For `shm://NAME`, the server creates the named shared-memory region and
  listens on the derived Unix-socket sidecar path. Each benchmark run connects
  through that sidecar, announces its active slot count, and then establishes
  one `ShmMessageStream` per active slot.
- When `--listen-uri=shm://NAME` is used, the server requires
  `--shm-slot-count=N` so the shared-memory layout has a fixed slot capacity.

### Lifecycle

- The server completes its transport-specific startup before notifying the
  benchmark parent that spawn-local startup is ready.
- Unless `--quiet` is set, it prints one startup banner with the bound or
  configured URI.
- `SIGINT` and `SIGTERM` are captured through KJ's Unix event port and cancel
  server work from within the event loop.
- After shutdown is requested, the process exits the event loop and terminates.

## Benchmark Semantics

### Run modes

- `connect` benchmarks one already running server over `tcp://...`,
  `unix://...`, or `shm://NAME`.
- `spawn-local` starts one local `rpc-bench-server` child, waits for startup
  readiness, runs the workload, and then stops the child.

Every benchmark invocation targets exactly one URI.

The benchmark CLI keeps separate `--connect-uri` and `--listen-uri` flags so
the selected run mode still determines whether it attaches to an existing
server or spawns one local child.

### Spawn-local transport setup

- For `tcp://...` and `unix://...`, the benchmark passes `--listen-uri=URI` to
  the child and waits for a startup-ready signal.
- For `pipe://socketpair`, the benchmark creates one socketpair per worker,
  inherits the server ends into the child, and keeps the parent ends for the
  worker threads.
- For `shm://NAME`, the benchmark passes `--listen-uri=shm://NAME` and
  `--shm-slot-count=client_threads` to the child, waits for startup readiness,
  connects to the derived Unix-socket sidecar, and then attaches one worker
  slot per client thread.

### Client concurrency model

- `client_threads` means one OS thread per logical benchmark worker.
- Each worker thread owns one KJ event loop.
- Each worker thread owns one RPC session for the entire run.
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

- `src/protocol/`: schema generation, payload-limit validation, and CRC32
  implementation
- `src/transport/`: URI parsing, session setup, KJ listener/runtime plumbing,
  spawn-local transport orchestration, and shared-memory helpers
- `src/server/`: CLI parsing and bootstrap capability wiring
- `src/bench/`: CLI parsing, workload generation, RPC client loops, and report
  rendering

The protocol layer is internal-only. This repository does not expose or install
a public SDK surface in this version.

## Design Decisions And Trade-offs

### Why keep every server transport on one KJ event loop

The benchmark is intended to compare transport paths without changing the basic
server execution model. Keeping all transports on one KJ event loop removes a
major source of runtime variation and keeps shutdown behavior consistent.

### Why use `TwoPartyVatNetwork` plus `RpcSystem` everywhere

Using the same RPC plumbing for stream-backed transports and custom
message-stream transports keeps the hash service implementation transport-agnostic.
The trade-off is that even the local-only transports still speak Cap'n Proto RPC
messages internally rather than a smaller custom protocol.

### Why `pipe://socketpair` is spawn-local only

`pipe://socketpair` depends on benchmark-owned process setup:

- `pipe://socketpair` requires inherited pre-connected descriptors

Restricting it to spawn-local mode keeps the public CLI simple and avoids
defining an external descriptor handoff protocol.

### Why shared memory stays hybrid instead of pure polling

The shared-memory transport moves message bytes through rings, but it still uses
a derived Unix-socket sidecar for attach, init, and wake notifications. That
keeps the server driven by the KJ event loop instead of introducing a non-KJ
polling loop just for shared memory.

### Why keep one outstanding request per worker

One in-flight request per worker keeps RTT semantics unambiguous and matches the
benchmark's user-visible concurrency model. The trade-off is that this version
does not explore pipelining or open-loop traffic generation.

### Why preserve legacy byte counters

The transport changed, but the terminal and JSON report fields remain stable so
existing users can compare runs without a report-schema migration. The trade-off
is that `requestBytes` and `responseBytes` are logical compatibility metrics
rather than literal on-the-wire Cap'n Proto sizes.

## Non-goals For This Milestone

- multi-threaded server execution
- multiple targets in one benchmark invocation
- persistence or recovery semantics
- remote or non-POSIX variants of `pipe://socketpair` and `shm://NAME`
- a public reusable client or server SDK
- repo-managed automated tests or coverage-report plumbing
