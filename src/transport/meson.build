rpcbench_transport_inc = include_directories('..')

rpcbench_transport = static_library(
  'rpcbench_transport',
  files(
    'benchmark_transport.cpp',
    'rpc_session.cpp',
    'server_transport.cpp',
    'shm_transport.cpp',
    'uri.cpp',
  ),
  include_directories : [rpcbench_transport_inc],
  dependencies : [rpcbench_protocol_dep],
)

rpcbench_transport_dep = declare_dependency(
  link_with : rpcbench_transport,
  include_directories : [rpcbench_transport_inc],
  dependencies : [rpcbench_protocol_dep],
)
