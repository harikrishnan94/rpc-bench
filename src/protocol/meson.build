asio_dep = dependency(
  'asio',
  fallback : ['asio', 'asio_dep'],
)
threads_dep = dependency('threads')

rpcbench_protocol_inc = include_directories('..')

rpcbench_protocol = static_library(
  'rpcbench_protocol',
  files(
    'common.cpp',
    'crc32.cpp',
  ),
  include_directories : [rpcbench_protocol_inc],
  dependencies : [asio_dep, threads_dep],
)

rpcbench_protocol_dep = declare_dependency(
  link_with : rpcbench_protocol,
  include_directories : [rpcbench_protocol_inc],
  dependencies : [asio_dep, threads_dep],
)
