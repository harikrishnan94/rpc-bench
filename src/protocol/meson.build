rpcbench_protocol_inc = include_directories('..')
rpcbench_schema = custom_target(
  'hash_service_schema',
  input : 'hash_service.capnp',
  output : ['hash_service.capnp.h', 'hash_service.capnp.c++'],
  command : [
    capnp_prog,
    'compile',
    '-o',
    capnpc_cpp_prog.full_path() + ':@OUTDIR@',
    '--src-prefix',
    meson.current_source_dir(),
    '-I',
    meson.current_source_dir(),
    '-I',
    capnp_include_dir,
    '@INPUT@',
  ],
)

rpcbench_protocol = static_library(
  'rpcbench_protocol',
  files(
    'common.cpp',
    'crc32.cpp',
  ),
  rpcbench_schema,
  include_directories : [rpcbench_protocol_inc],
  dependencies : [threads_dep, capnp_dep, capnp_rpc_dep, kj_dep, kj_async_dep],
)

rpcbench_protocol_dep = declare_dependency(
  link_with : rpcbench_protocol,
  include_directories : [rpcbench_protocol_inc],
  dependencies : [threads_dep, capnp_dep, capnp_rpc_dep, kj_dep, kj_async_dep],
  sources : [rpcbench_schema[0]],
)
