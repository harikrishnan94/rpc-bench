rpcbench_app_inc = include_directories('..')

rpcbench_app = static_library(
  'rpcbench_app',
  files('application.cpp'),
  include_directories : [rpcbench_app_inc],
  dependencies : [
    rpcbench_bench_runtime_dep,
    rpcbench_server_runtime_dep,
    rpcbench_transport_dep,
  ],
)

rpcbench_app_dep = declare_dependency(
  link_with : rpcbench_app,
  include_directories : [rpcbench_app_inc],
  dependencies : [
    rpcbench_bench_runtime_dep,
    rpcbench_server_runtime_dep,
    rpcbench_transport_dep,
  ],
)
