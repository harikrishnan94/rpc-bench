project(
  'rpc-bench',
  'cpp',
  version : '0.1.0',
  default_options : [
    'cpp_std=c++23',
    'warning_level=3',
  ],
)

python = import('python').find_installation(required : true)

capnp_prog = find_program('capnp', required : false)
capnpc_cpp_prog = find_program('capnpc-c++', required : false)
pkg_config_prog = find_program('pkg-config', required : false)

capnp_dep = dependency(
  'capnp',
  method : 'pkg-config',
  allow_fallback : false,
  required : false,
)
capnp_rpc_dep = dependency(
  'capnp-rpc',
  method : 'pkg-config',
  allow_fallback : false,
  required : false,
)
kj_dep = dependency(
  'kj',
  method : 'pkg-config',
  allow_fallback : false,
  required : false,
)
kj_async_dep = dependency(
  'kj-async',
  method : 'pkg-config',
  allow_fallback : false,
  required : false,
)
threads_dep = dependency('threads')

capnp_missing = []
if not capnp_prog.found()
  capnp_missing += ['- missing `capnp` on `PATH`']
endif
if not capnpc_cpp_prog.found()
  capnp_missing += ['- missing `capnpc-c++` on `PATH`']
endif
if not pkg_config_prog.found()
  capnp_missing += ['- missing `pkg-config` on `PATH`']
endif
if not capnp_dep.found()
  capnp_missing += ['- missing pkg-config dependency `capnp`']
endif
if not capnp_rpc_dep.found()
  capnp_missing += ['- missing pkg-config dependency `capnp-rpc`']
endif
if not kj_dep.found()
  capnp_missing += ['- missing pkg-config dependency `kj`']
endif
if not kj_async_dep.found()
  capnp_missing += ['- missing pkg-config dependency `kj-async`']
endif

if capnp_missing.length() > 0
  error(
    'Cap\'n Proto system toolchain is required for rpc-bench.\n'
    + 'Missing items:\n'
    + '\n'.join(capnp_missing)
    + '\n\n'
    + 'The build needs system-installed `capnp`, `capnpc-c++`, and pkg-config-visible\n'
    + '`capnp`, `capnp-rpc`, `kj`, and `kj-async`.\n\n'
    + 'If Cap\'n Proto is already installed, expose it through `PATH` and `PKG_CONFIG_PATH`\n'
    + 'before running Meson again.\n\n'
    + 'If it is not installed yet, follow the official install guide. Common examples:\n'
    + '  brew install capnp\n'
    + '  apt-get install capnproto\n'
    + '  pacman -S capnproto\n'
  )
endif

capnp_include_dir = capnp_dep.get_variable(pkgconfig : 'includedir')

subdir('src')
subdir('tests')

run_target(
  'coverage-report',
  command : [
    python,
    files('scripts/coverage.py'),
  ],
)
