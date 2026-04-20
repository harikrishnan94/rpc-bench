rpcbench_bench_inc = include_directories('..')

rpcbench_bench_runtime = static_library(
  'rpcbench_bench_runtime',
  files('benchmark.cpp'),
  include_directories : [rpcbench_bench_inc],
  dependencies : [rpcbench_protocol_dep],
)

rpcbench_bench_runtime_dep = declare_dependency(
  link_with : rpcbench_bench_runtime,
  include_directories : [rpcbench_bench_inc],
  dependencies : [rpcbench_protocol_dep],
)
