rpc_bench_tests = executable(
  'rpc-bench-tests',
  files('test_main.cpp'),
  dependencies : [rpcbench_app_dep],
)

test(
  'rpc-bench-tests',
  rpc_bench_tests,
  env : [
    'RPCBENCH_BENCH_PATH=' + bench_exe.full_path(),
    'RPCBENCH_SERVER_PATH=' + server_exe.full_path(),
  ],
)

benchmark(
  'rpc-bench-smoke',
  bench_exe,
  args : [
    '--mode=spawn-local',
    '--listen-uri=pipe://socketpair',
    '--server-threads=1',
    '--client-threads=2',
    '--client-connections=3',
    '--message-size-min=16',
    '--message-size-max=64',
    '--warmup-seconds=0.05',
    '--measure-seconds=0.10',
    '--seed=1',
    '--quiet-server',
  ],
)
