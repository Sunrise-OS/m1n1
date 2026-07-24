# Port of the top-level Makefile object list.

chickens_src = files(
    'chickens/avalanche.c',
    'chickens/blizzard.c',
    'chickens/cyclone_typhoon.c',
    'chickens/everest.c',
    'chickens/firestorm.c',
    'chickens/hurricane_zephyr.c',
    'chickens/icestorm.c',
    'chickens/monsoon_mistral.c',
    'chickens/sawtooth.c',
    'chickens/twister.c',
)

dcp_src = files(
    'dcp/dpav_ep.c',
    'dcp/dptx_phy.c',
    'dcp/dptx_port_ep.c',
    'dcp/parser.c',
    'dcp/system_ep.c',
)

minilzlib_src = files(
    'minilzlib/dictbuf.c',
    'minilzlib/inputbuf.c',
    'minilzlib/lzma2dec.c',
    'minilzlib/lzmadec.c',
    'minilzlib/rangedec.c',
    'minilzlib/xzstream.c',
)

tinf_src = files(
    'tinf/adler32.c',
    'tinf/crc32.c',
    'tinf/tinfgzip.c',
    'tinf/tinflate.c',
    'tinf/tinfzlib.c',
)

dlmalloc_src = files('dlmalloc/malloc.c')

libfdt_src = files(
    'libfdt/fdt_addresses.c',
    'libfdt/fdt_empty_tree.c',
    'libfdt/fdt_ro.c',
    'libfdt/fdt_rw.c',
    'libfdt/fdt_strerror.c',
    'libfdt/fdt_sw.c',
    'libfdt/fdt_wip.c',
    'libfdt/fdt.c',
)

main_asm_src = files(
    'chainload_asm.S',
    'exception_asm.S',
    'gxf_asm.S',
    'hv_asm.S',
    'memory_asm.S',
    'start.S',
    'utils_asm.S',
)

main_src = files(
    'adt.c',
    'afk.c',
    'aic.c',
    'asc.c',
    'chainload.c',
    'chickens.c',
    'clk.c',
    'cpufreq.c',
    'dapf.c',
    'dart.c',
    'dcp.c',
    'dcp_iboot.c',
    'devicetree.c',
    'display.c',
    'dockchannel_uart.c',
    'exception.c',
    'fb.c',
    'firmware.c',
    'gxf.c',
    'heapblock.c',
    'hv.c',
    'hv_vm.c',
    'hv_exc.c',
    'hv_vuart.c',
    'hv_wdt.c',
    'hv_aic.c',
    'hv_virtio.c',
    'hv_sprr.c',
    'i2c.c',
    'iodev.c',
    'iova.c',
    'isp.c',
    'kboot.c',
    'kboot_atc.c',
    'kboot_t6020_compat.c',
    'main.c',
    'mitigations.c',
    'mcc.c',
    'memory.c',
    'nvme.c',
    'payload.c',
    'pcie.c',
    'pmgr.c',
    'proxy.c',
    'ringbuffer.c',
    'rtkit.c',
    'sart.c',
    'sep.c',
    'sio.c',
    'smc.c',
    'smp.c',
    'spmi.c',
    'startup.c',
    'string.c',
    'tunables.c',
    'tunables_static.c',
    'tps6598x.c',
    'uart.c',
    'uartproxy.c',
    'usb.c',
    'usb_dwc3.c',
    'utils.c',
    'vsprintf.c',
    'wdt.c',
) + chickens_src + dcp_src + minilzlib_src + tinf_src + dlmalloc_src + libfdt_src

# Objects that must NOT be built with -mgeneral-regs-only (uses FP for math).
fp_src = files(
    'kboot_gpu.c',
    'math/expf.c',
    'math/exp2f_data.c',
    'math/powf.c',
    'math/powf_data.c',
)

extra_c_args = [
    '-I' + meson.project_source_root(),
    '-include',
    build_cfg_h.full_path(),
]

# --- embedded binary assets -> objcopy'd .o -------------------------------
# fb.c references symbols like `_binary_build_bootlogo_48_bin_start`, which
# is the mangled name objcopy derives from the *relative input path* it was
# given (`build/bootlogo_48.bin` in the old Makefile, which staged assets
# into build/ first). Reproduce that exact layout/naming here.
sh = find_program('sh')
asset_names = [
    'bootlogo_48.bin',
    'bootlogo_128.bin',
    'bootlogo_256.bin',
    'font.bin',
    'font_retina.bin',
]
asset_srcs = files(
    '../data/bootlogo_48.bin',
    '../data/bootlogo_128.bin',
    '../data/bootlogo_256.bin',
    '../font/font.bin',
    '../font/font_retina.bin',
)

asset_objs = []
foreach i : [0, 1, 2, 3, 4]
    name = asset_names[i]
    asset_objs += custom_target(
        name + '.o',
        input: asset_srcs[i],
        output: name + '.o',
        command: [
            sh,
            '-c',
            'mkdir -p build && cp "$1" "build/$2" && ' + objcopy.full_path() + ' -I binary -B aarch64 -O elf64-littleaarch64 "build/$2" "$3"',
            '_',
            '@INPUT@',
            name,
            '@OUTPUT@',
        ],
    )
endforeach

# --- main + fp static libs -------------------------------------------------
main_lib = static_library(
    'm1n1-main',
    main_src + [build_tag_h],
    c_args: extra_c_args + ['-mgeneral-regs-only'],
    include_directories: inc,
)

fp_lib = static_library(
    'm1n1-fp',
    fp_src + [build_tag_h],
    c_args: extra_c_args,
    include_directories: inc,
)

# Assembly sources must NOT get -mgeneral-regs-only (some use FP/SIMD regs
# for context save/restore), matching the Makefile's BASE_CFLAGS-only rule
# for %.S files.
asm_lib = static_library(
    'm1n1-asm',
    main_asm_src,
    c_args: extra_c_args,
    include_directories: inc,
)

all_objs = [asset_objs]

m1n1_elf = executable(
    'm1n1.elf',
    objects: all_objs,
    link_whole: [main_lib, fp_lib, asm_lib, rust_lib],
    link_args: link_args + ['-Wl,-T,' + meson.project_source_root() / 'm1n1.ld'],
    link_depends: files('../m1n1.ld'),
    build_by_default: false,
)

m1n1_raw_elf = executable(
    'm1n1-raw.elf',
    objects: all_objs,
    link_whole: [main_lib, fp_lib, asm_lib, rust_lib],
    link_args: link_args + [
        '-Wl,-T,' + meson.project_source_root() / 'm1n1-raw.ld',
    ],
    link_depends: files('../m1n1-raw.ld'),
    build_by_default: false,
)

m1n1_macho = custom_target(
    'm1n1.macho',
    input: m1n1_elf,
    output: 'm1n1.macho',
    command: [objcopy, '-O', 'binary', '--strip-debug', '@INPUT@', '@OUTPUT@'],
    install: false,
    build_by_default: true,
)

m1n1_bin = custom_target(
    'm1n1.bin',
    input: m1n1_raw_elf,
    output: 'm1n1.bin',
    command: [objcopy, '-O', 'binary', '--strip-debug', '@INPUT@', '@OUTPUT@'],
    install: false,
    build_by_default: true,
)
