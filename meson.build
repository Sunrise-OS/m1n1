project(
    'm1n1',
    'c',
    default_options: ['c_std=none', 'warning_level=0', 'b_lundef=false'],
    meson_version: '>=1.4.0',
)

if not (
    host_machine.system() == 'none'
    and host_machine.cpu_family() == 'aarch64'
)
    error(
        'm1n1 must be built with an aarch64 bare-metal cross file, e.g.:\n' + '  meson setup build --cross-file cross/aarch64-clang.ini\n' + '  meson setup build --cross-file cross/aarch64-linux-gnu.ini',
    )
endif

add_languages('rust', native: false, required: true)
add_languages('rust', native: true, required: true)

cc = meson.get_compiler('c')
objcopy = find_program('objcopy', 'llvm-objcopy')

release = get_option('release')
chainloading = get_option('chainloading')

cfg_defines = []
if release
    cfg_defines += 'RELEASE'
endif
if chainloading
    cfg_defines += 'CHAINLOADING'
endif

cfg_lines = []
foreach d : cfg_defines
    cfg_lines += '#define @0@\n'.format(d)
endforeach
build_cfg_h = configure_file(
    output: 'build_cfg.h',
    configuration: configuration_data(),
)
# configure_file needs actual content; write it directly.
run_command(
    find_program('sh'),
    '-c',
    'printf "%s" "$1" > "$2"',
    'build_cfg_h',
    ''.join(cfg_lines),
    build_cfg_h.full_path(),
    check: true,
)

build_tag_h = custom_target(
    'build_tag_h',
    output: 'build_tag.h',
    command: [find_program('version.sh')],
    capture: true,
    build_always_stale: true,
)
# version.sh prints `#define BUILD_TAG "..."` directly, matches Makefile usage.

# The compiler's own freestanding headers (stdbool.h, stdarg.h, stddef.h,
# ...); matches `-isystem $($(CC) -print-file-name=include)` in the old
# Makefile.
clang_include_dir = run_command(
    cc.cmd_array()[0],
    '-print-file-name=include',
    check: true,
).stdout().strip()

base_c_args = [
    '-Wall',
    '-Wundef',
    '-Werror=strict-prototypes',
    '-fno-common',
    '-fno-PIE',
    '-Werror=implicit-function-declaration',
    '-Werror=implicit-int',
    '-Wsign-compare',
    '-Wunused-parameter',
    '-Wno-multichar',
    '-ffreestanding',
    '-fpic',
    '-ffunction-sections',
    '-fdata-sections',
    '-nostdinc',
    # Use -idirafter (not -isystem) for the compiler's own freestanding
    # headers (stdbool.h etc.): meson silently strips '-isystem <dir>' args
    # that point at a compiler's own default system include path, which
    # would normally be harmless but isn't here because we pass -nostdinc.
    '-idirafter',
    clang_include_dir,
    '-isystem@0@'.format(meson.current_source_dir() / 'sysinc'),
    '-fno-stack-protector',
    '-mstrict-align',
    '-march=armv8.2-a',
]

base_c_args += ['-O2', '-g']

add_project_arguments(base_c_args, language: 'c')

inc = include_directories('src')

link_args = [
    '-Wl,-EL',
    '-Wl,-maarch64elf',
    '-Wl,--no-undefined',
    '-Wl,-X',
    '-Wl,-Bsymbolic',
    '-Wl,-z,notext',
    '-Wl,--no-apply-dynamic-relocs',
    '-Wl,--orphan-handling=warn',
    '-Wl,-z,nocopyreloc',
    '-Wl,--gc-sections',
    '-Wl,-pie',
    '-nostdlib',
]

subdir('rust')
subdir('src')

# Convenience top-level copies matching the old Makefile's `build/m1n1.macho`
# and `build/m1n1.bin` paths (actual targets live under build/src/).
custom_target(
    'm1n1.macho-toplevel',
    input: m1n1_macho,
    output: 'm1n1.macho',
    command: ['cp', '@INPUT@', '@OUTPUT@'],
    build_by_default: true,
)
custom_target(
    'm1n1.bin-toplevel',
    input: m1n1_bin,
    output: 'm1n1.bin',
    command: ['cp', '@INPUT@', '@OUTPUT@'],
    build_by_default: true,
)
