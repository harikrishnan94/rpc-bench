rpc_bench_core_sources = files(
  'bench.cpp',
  'config.cpp',
  'metrics.cpp',
  'rpc.cpp',
  'storage.cpp',
)

rpc_bench_core = library(
  'rpc_bench_core',
  rpc_bench_core_sources,
  include_directories : [project_inc, generated_inc],
  dependencies : [rpc_bench_schema_dep, capnp_rpc_dep, kj_async_dep],
)

rpc_bench_core_dep = declare_dependency(
  link_with : rpc_bench_core,
  include_directories : [project_inc, generated_inc],
  dependencies : [rpc_bench_schema_dep, capnp_rpc_dep, kj_async_dep],
)

server_exe = executable(
  'rpc-bench-server',
  files('server_main.cpp'),
  include_directories : [project_inc, generated_inc],
  dependencies : [rpc_bench_core_dep],
)

bench_exe = executable(
  'rpc-bench-bench',
  files('bench_main.cpp'),
  include_directories : [project_inc, generated_inc],
  dependencies : [rpc_bench_core_dep],
)
