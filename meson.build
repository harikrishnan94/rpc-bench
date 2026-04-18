project(
  'rpc-bench',
  'cpp',
  version : '0.1.0',
  default_options : [
    'cpp_std=c++23',
    'warning_level=3',
  ],
)

cmake = import('cmake')
python = import('python').find_installation(required : true)

project_inc = include_directories('include')
generated_inc = include_directories('.')
schema_source_dir = join_paths(meson.project_source_root(), 'schemas')

capnp_opts = cmake.subproject_options()
capnp_opts.set_install(false)
capnp_opts.add_cmake_defines({
  'BUILD_TESTING' : false,
  'BUILD_SHARED_LIBS' : false,
  'WITH_OPENSSL' : 'OFF',
  'WITH_ZLIB' : 'OFF',
  'WITH_FIBERS' : 'OFF',
})

capnp_sp = cmake.subproject('capnproto', options : capnp_opts)

capnp_dep = capnp_sp.dependency('capnp')
capnp_rpc_dep = capnp_sp.dependency('capnp-rpc')
kj_dep = capnp_sp.dependency('kj')
kj_async_dep = capnp_sp.dependency('kj-async')

capnp_tool = capnp_sp.target('capnp_tool')
capnpc_cpp = capnp_sp.target('capnpc_cpp')

kv_schema = custom_target(
  'kv_schema',
  input : files('schemas/kv.capnp'),
  output : [
    'kv.capnp.h',
    'kv.capnp.c++',
  ],
  command : [
    capnp_tool,
    'compile',
    '-o',
    capnpc_cpp.full_path() + ':@OUTDIR@',
    '--src-prefix',
    schema_source_dir,
    '-I',
    schema_source_dir,
    '@INPUT@',
  ],
  depends : [capnpc_cpp],
)

rpc_bench_schema = static_library(
  'rpc_bench_schema',
  kv_schema,
  include_directories : [generated_inc],
  dependencies : [capnp_dep, kj_dep],
)

rpc_bench_schema_dep = declare_dependency(
  link_with : rpc_bench_schema,
  include_directories : [generated_inc],
  sources : kv_schema,
  dependencies : [capnp_dep, kj_dep],
)

subdir('src')
subdir('tests')

run_target(
  'coverage-report',
  command : [
    python,
    files('scripts/coverage.py'),
  ],
)
