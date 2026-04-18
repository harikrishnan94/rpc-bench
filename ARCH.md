# rpc-bench Architecture

## Overview

`rpc-bench` measures the performance of a remote key-value service implemented
with Cap'n Proto RPC. The observable system consists of:

- A key-value RPC interface with `Get`, `Put`, and `Delete`.
- A server executable that exposes exactly one shard on exactly one endpoint.
- A benchmark executable that can either connect to one existing endpoint or
  spawn one local server process.
- A reporting layer that emits human-readable summaries and machine-readable
  JSON for each benchmark run.

The project is intentionally structured so the benchmark logic, reporting, and
configuration parsing are reusable without committing to a persistent storage
engine yet.

## Behavioral Model

### Key-value semantics

- Keys and values are opaque byte strings.
- `Put` stores or replaces the value for a key.
- `Get` returns `found=false` with an empty value when the key is absent.
- `Get` returns `found=true` and the stored bytes when the key is present.
- `Delete` removes the key if it exists and reports whether removal happened.

The v1 implementation uses an in-memory map. Restarting a server discards all
data. Persistence and crash recovery are explicitly out of scope for this
revision.

### Endpoint and thread model

Each server process owns a single event loop, a single Cap'n Proto listener,
and a single in-memory shard. Each benchmark run targets exactly one endpoint.

This choice preserves three properties that matter for reproducible benchmarks:

- One Cap'n Proto EzRpc event loop is owned by one thread.
- The benchmark never hides sharding behind a multi-endpoint run.
- Remote and spawn-local runs share the same single-endpoint execution shape.

All client threads talk to the same endpoint for the lifetime of a run. Every
thread still owns a distinct logical key-space, so `Get` and `Delete`
operations observe real key-value semantics without requiring cross-thread
client sharing.

The current server implementation is single-threaded and async. In spawn-local
mode the benchmark can request `server_threads`, but the server warns and falls
back to one effective server thread when a value greater than `1` is supplied.
Connect mode does not allow the benchmark to configure remote server thread
counts.

## RPC Contract

The wire protocol is defined in `schemas/kv.capnp`.

- `Get(key)` returns `found` and `value`.
- `Put(key, value)` stores bytes and returns no payload.
- `Delete(key)` returns `found`.

The benchmark relies on this interface being stable. Any future reimplementation
in another language must preserve request and response semantics exactly if it
claims wire compatibility with this version.

## Benchmark Model

### Load generation

The benchmark uses a bounded in-flight model:

- Each client thread owns one EzRpc client and one event loop.
- Each client thread maintains up to `queue_depth` outstanding RPC chains.
- As soon as one request completes, the same chain issues the next request
  until the phase deadline is reached.

This is a closed-loop workload. Throughput therefore reflects service time,
network time, queue depth, and client backpressure together.

### Workload contents

The workload is defined by:

- Client thread-count sweep.
- Queue-depth sweep.
- Key-size sweep.
- Value-size sweep.
- One or more operation mixes expressed as `get:put:delete` percentages.
- Warmup duration.
- Measured duration.
- Iteration count.
- Seed and key-space size.

Spawn-local mode also accepts one requested server-thread count. That setting
is not a sweep in this version, and the current implementation always executes
with one effective server thread after any required fallback.

Each iteration begins with a prefill stage that inserts the thread-local
key-space for every participating client thread. Prefill is not counted toward
reported throughput or latency.

### Reporting

For every matrix point and iteration the benchmark reports:

- Effective server thread count.
- Endpoint used for the run.
- Total operation count.
- Per-operation counts for get, put, and delete.
- Found and missing get counts.
- Removed and missing delete counts.
- Error count.
- Request and response payload bytes.
- Operations per second.
- MiB per second.
- Latency min, p50, p90, p99, and max.

The text report is intended for terminals and quick inspection. The JSON report
is intended for automation and later comparison tooling.

## Implementation Structure

The codebase is split into an internal core library plus runnable frontends:

- `config`: CLI parsing, validation, and usage text.
- `storage`: key-value abstractions and the in-memory backend.
- `rpc`: Cap'n Proto service implementation and server runner.
- `metrics`: benchmark result types plus text and JSON formatting.
- `bench`: workload execution, local process spawning, remote connection logic,
  and metric aggregation.

The core library is internal-only in this version. Headers live under
`include/rpcbench/` for code organization, but the project does not install a
public SDK or guarantee downstream source compatibility yet.

## Design Decisions And Trade-offs

### Why EzRpc first

EzRpc gives a correct and compact Cap'n Proto server/client integration point
for the first benchmarkable version. The trade-off is that transport control is
less flexible than lower-level two-party RPC APIs.

### Why explicit Meson source lists

Meson does not treat wildcard source discovery as a primary model. Explicit
source lists keep the build graph reviewable and make generated-code edges
obvious.

### Why single-endpoint runs for now

Single-endpoint runs keep ownership boundaries simple:

- One process owns one event loop.
- The benchmark result shape stays aligned with what actually executed.
- Remote deployment stays a first-class path without hidden sharding behavior.

The trade-off is that the benchmark does not yet model multi-endpoint or
multi-threaded server execution. The `server_threads` flag exists to reserve
the future interface, but values above `1` currently warn and fall back to the
single-threaded async server.

## Future Work

The architecture intentionally leaves room for:

- Persistent storage engines.
- Richer workload policies and open-loop traffic generation.
- Cross-host orchestration helpers.
- A real multi-threaded server implementation behind `server_threads`.
- Additional RPC methods, such as batched operations or range scans.

Any future change that alters wire semantics, sharding semantics, report
semantics, or lifecycle expectations must update this file.
