# rpc-bench Architecture

## Overview

`rpc-bench` measures one simple remote service: a unary RPC whose result is the
CRC32 hash of a request payload. The observable system consists of:

- an internal `protocol` library shared by both executables
- `rpc-bench-server`, a single-listen-target hash server
- `rpc-bench-bench`, a benchmark driver that targets one server at a time

The implementation is intentionally small and explicit. This version does not
define persistence, sharding, or multi-target runs.

An internal application layer owns CLI parsing and URI/mode policy. The bench
and server runtime layers below it operate on async stream abstractions rather
than on URI strings.

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
  affected benchmark thread fails the run.

## Transport Semantics

### Public URI surface

The user-visible transport identifier is always a URI.

Supported URI kinds:

- `tcp://HOST:PORT`
- `unix:///absolute/path`
- `pipe://socketpair`

### Transport boundary

- The application layer parses URIs and benchmark/server mode policy.
- The transport layer turns those URI policies into listener setup,
  spawn-local process wiring, and async stream openers.
- The bench and server runtime layers consume only async stream-backed RPC
  sessions and do not parse or branch on URI strings.

### Mode restrictions

- `connect` is defined for `tcp://...` and `unix://...`
- `spawn-local` is defined for `tcp://...`, `unix://...`, and
  `pipe://socketpair`
- `pipe://socketpair` remains `spawn-local` only because it depends on
  inherited pre-connected file descriptors

## Server Semantics

### Runtime model

- One server process owns one acceptor runtime plus one or more serving
  runtimes.
- For `tcp://...` and `unix://...`, the acceptor owns one KJ async event loop
  that binds the listener and accepts network streams.
- `server_threads` means the number of serving worker runtimes for
  `tcp://...` and `unix://...`. Each worker owns one KJ async event loop.
- Accepted network connections are assigned round-robin from the acceptor to
  the workers.
- For `pipe://socketpair`, the server stays on one KJ async event loop and
  does not create extra workers.
- Every accepted or pre-created session is serviced through one
  `TwoPartyVatNetwork` plus one `RpcSystem`.
- Each serving runtime exports one bootstrap capability that implements the
  hash RPC.

### Transport-specific session setup

- For `tcp://...` and `unix://...`, the server resolves the URI through KJ,
  binds one listener on the acceptor thread, accepts streams from that
  listener, duplicates the accepted fd, and asks the selected worker thread to
  wrap and serve that stream.
- For `pipe://socketpair`, the benchmark parent creates one socketpair per
  logical client connection before spawning the child server. The child wraps
  the inherited server ends into KJ streams and serves them as pre-connected
  RPC sessions.

### Server thread policy

- The public CLI keeps `--server-threads=N`.
- For `tcp://...` and `unix://...`, `N` counts serving workers only. The
  hidden acceptor thread is not included in that count.
- The benchmark rejects `--server-threads` in `connect` mode because it cannot
  configure an already-running server.
- For `pipe://socketpair`, the server process rejects values greater than `1`.
  The benchmark does not pre-empt that spawn-local failure.

### Lifecycle

- In spawn-local mode the server completes its transport-specific startup
  before notifying the benchmark parent through a ready pipe.
- For `tcp://...` and `unix://...`, readiness means the listener is bound and
  all serving workers have started.
- Unless `--quiet` is set, the server prints one startup banner with the bound
  URI.
- `SIGINT` and `SIGTERM` are captured through KJ's Unix event port and cancel
  server work from within the acceptor event loop.
- After shutdown is requested, the acceptor stops accepting, pending handoffs
  finish, and the serving workers exit.

## Benchmark Semantics

### Run modes

- `connect` benchmarks one already running server over `tcp://...` or
  `unix://...`
- `spawn-local` starts one local `rpc-bench-server` child, waits for startup
  readiness, runs the workload, and then stops the child

Every benchmark invocation targets exactly one URI.

### Spawn-local transport setup

- For `tcp://...` and `unix://...`, the benchmark passes `--listen-uri=URI` to
  the child and waits for a startup-ready signal.
- For `pipe://socketpair`, the benchmark creates one socketpair per logical
  client connection, inherits the server ends into the child, and keeps the
  parent ends for the benchmark runtime.

### Client concurrency model

- `client_threads` means the number of benchmark event-loop threads.
- Each thread owns one KJ event loop.
- `client_connections` means the total number of RPC sessions opened for the
  run.
- Connections are distributed as evenly as possible across the configured
  threads. Some threads may host zero connections.
- Each connection keeps exactly one request outstanding at a time.
- This is a closed-loop benchmark. A connection sends the next RPC only after
  the previous reply arrives.

### Workload contents

- Request payload sizes are sampled uniformly from an inclusive size range.
- The default range is `128-256` bytes.
- Payload contents are generated from a deterministic PRNG stream derived from
  the configured seed and the logical connection index.
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
- `src/app/`: CLI parsing and top-level orchestration
- `src/transport/`: URI parsing, spawn-local wiring, listener setup, and
  stream session plumbing
- `src/server/`: bootstrap capability implementation
- `src/bench/`: benchmark runtime, workload generation, and report rendering

The protocol layer is internal-only. This repository does not expose or install
a public SDK surface in this version.

## Design Decisions And Trade-offs

### Why use one acceptor plus worker loops for network transports

The benchmark needs a real `--server-threads=N` network path without moving KJ
objects across threads unsafely. Keeping listener ownership on one acceptor
thread and handing accepted fds to worker event loops preserves KJ's
thread-local event-loop model while still allowing parallel request servicing.
The trade-off is one hidden extra thread plus an fd-duplication handoff on each
accepted network connection.

### Why keep the runtime stream-only

The user-facing architecture goal for this milestone is that bench and server
runtime code should talk in terms of KJ async streams rather than in terms of
transport-specific URI branching. The trade-off is that the application and
transport layers must carry a little more setup logic.

### Why use `TwoPartyVatNetwork` plus `RpcSystem`

Using the same RPC plumbing for every stream-backed transport keeps the hash
service implementation transport-agnostic. The trade-off is that even local
pipe-based runs still speak Cap'n Proto RPC messages internally.

### Why `pipe://socketpair` is spawn-local only

`pipe://socketpair` depends on benchmark-owned process setup:

- `pipe://socketpair` requires inherited pre-connected descriptors

Restricting it to spawn-local mode keeps the public CLI simple and avoids
defining an external descriptor handoff protocol.

### Why keep `pipe://socketpair` single-threaded

`pipe://socketpair` already models one pre-connected stream per logical client
connection. Keeping that transport on one event loop avoids inventing a second
handoff layer for inherited descriptors when the benchmark's main threading
goal is network-server parallelism.

### Why keep one outstanding request per connection

One in-flight request per connection keeps RTT semantics unambiguous while
still allowing concurrency through multiple connections per event-loop thread.
The trade-off is that this version does not explore per-connection pipelining
or open-loop traffic generation.

### Why preserve legacy byte counters

The transport changed, but the terminal and JSON report fields remain stable so
existing users can compare runs without a report-schema migration. The
trade-off is that `requestBytes` and `responseBytes` are logical compatibility
metrics rather than literal on-the-wire Cap'n Proto sizes.

## Non-goals For This Milestone

- multiple targets in one benchmark invocation
- persistence or recovery semantics
- any shared-memory transport
- remote or non-POSIX variants of `pipe://socketpair`
- a public reusable client or server SDK
