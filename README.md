# rpc-bench

`rpc-bench` is a Cap'n Proto RPC benchmark for a key-value service. The project
builds a standalone server plus a benchmark driver that measures achievable
transactions per second, latency percentiles, and payload bandwidth across
different client thread counts, queue depths, payload sizes, and one
single-endpoint server configuration.

## Status

The current bootstrap implements:

- A real in-memory key-value backend.
- A Cap'n Proto RPC schema plus generated-code build wiring.
- `rpc-bench-server`, which serves a single shard on one endpoint.
- `rpc-bench-bench`, which runs remote or local-spawn benchmark runs against
  one endpoint and emits text plus JSON reports.
- `rpc-bench-tests`, which covers parsing, metrics, storage, and a loopback
  benchmark integration path.

Persistence, replication, and richer workload policies are intentionally out of
scope for this first version. Those behaviors are described in
`ARCH.md` as future extension points rather than implemented features.

## Build

Use the repository's local Meson native files. The default debug workflow is:

```sh
meson setup builddir --native-file native/clang22-debug.ini
meson compile -C builddir
```

The first configure downloads Cap'n Proto from the official source release via
the custom wrap in `subprojects/`.

## Test

Run the unit and integration tests with Meson:

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

## Binaries

### `rpc-bench-server`

The server binds one endpoint and serves one in-memory shard:

```sh
./builddir/src/rpc-bench-server --listen-host=127.0.0.1 --port=7000
```

Important notes:

- Each server process owns one event loop and one shard.
- `--server-threads` accepts values above `1`, but the server warns and falls
  back to the current single-threaded async implementation.
- The server does not persist data across restarts in this version.

### `rpc-bench-bench`

The benchmark runner supports two modes:

- `connect`: connect to one already running server endpoint.
- `spawn-local`: launch one local `rpc-bench-server` process automatically.

Example local run:

```sh
./builddir/src/rpc-bench-bench \
  --mode=spawn-local \
  --server-port=7300 \
  --server-threads=1 \
  --client-threads=1,2,4,8 \
  --queue-depths=1,8,32 \
  --key-sizes=16 \
  --value-sizes=128,1024 \
  --mixes=80:19:1,50:50:0 \
  --warmup-seconds=0.5 \
  --measure-seconds=1.5 \
  --iterations=2 \
  --json-output=report.json
```

If `spawn-local` receives `--server-threads` greater than `1`, the server warns
to `stderr` and defaults to the current single-threaded async server.

Example remote run against one endpoint:

```sh
./builddir/src/rpc-bench-bench \
  --mode=connect \
  --endpoint=127.0.0.1:7000 \
  --client-threads=4,8 \
  --queue-depths=8,32 \
  --key-sizes=16 \
  --value-sizes=512 \
  --mixes=90:10:0
```

## Adding Files

Meson source lists are explicit by design. Adding new `.cpp`, `.hpp`, or
`.capnp` files will require updating the relevant `meson.build` file. This
keeps the build graph auditable and matches Meson's documented source-discovery
model.
