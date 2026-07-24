# Rust support library (build/librust.a), built entirely via Meson's native
# Rust target support (no cargo invocation). External crates are pulled in
# as Meson subprojects using Meson's Cargo-wrap support (see ../subprojects).

rustmod = import('rust')

rust_args = []
if chainloading
  rust_args += '--cfg=feature="chainload"'
endif

# local proc-macro crate (rust/versions)
versions_lib = rustmod.proc_macro('versions',
  'versions/src/lib.rs',
  rust_args: ['--edition=2024'],
)

rust_deps = []

if chainloading
  fatfs_dep = dependency('fatfs-0.4-rs', fallback: ['fatfs-rs', 'fatfs-0.4-rs'])
  uuid_dep = dependency('uuid-1-rs', fallback: ['uuid-rs', 'uuid-1-rs'])
  rust_deps += [fatfs_dep, uuid_dep]
  rust_args += ['--cfg=feature="alloc"', '--cfg=feature="lfn"']
endif

rust_lib = static_library('rust',
  'src/lib.rs',
  rust_abi: 'c',
  rust_args: [
    '--edition=2021',
    '--cfg=feature_versions',
  ] + rust_args,
  link_with: [versions_lib],
  dependencies: rust_deps,
  native: false,
)
