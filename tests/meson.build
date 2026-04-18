rpc_bench_tests = executable(
  'rpc-bench-tests',
  files('test_main.cpp'),
  include_directories : [project_inc, generated_inc],
  dependencies : [rpc_bench_core_dep],
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
    '--server-port=47300',
    '--server-threads=1',
    '--client-threads=1',
    '--queue-depths=1',
    '--key-sizes=16',
    '--value-sizes=64',
    '--mixes=80:20:0',
    '--warmup-seconds=0.05',
    '--measure-seconds=0.10',
    '--iterations=1',
    '--key-space=32',
    '--quiet-server',
  ],
)
