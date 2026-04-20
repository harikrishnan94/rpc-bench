rpcbench_server_inc = include_directories('..')

rpcbench_server_runtime = static_library(
  'rpcbench_server_runtime',
  files('server.cpp'),
  include_directories : [rpcbench_server_inc],
  dependencies : [rpcbench_protocol_dep],
)

rpcbench_server_runtime_dep = declare_dependency(
  link_with : rpcbench_server_runtime,
  include_directories : [rpcbench_server_inc],
  dependencies : [rpcbench_protocol_dep],
)
