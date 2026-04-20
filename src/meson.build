subdir('protocol')
subdir('bench')
subdir('server')
subdir('transport')
subdir('app')

server_exe = executable(
  'rpc-bench-server',
  files('server/main.cpp'),
  dependencies : [rpcbench_app_dep],
)

bench_exe = executable(
  'rpc-bench-bench',
  files('bench/main.cpp'),
  dependencies : [rpcbench_app_dep],
)
