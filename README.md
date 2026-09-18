# m1n1: A bootloader and experimentation playground for Apple Silicon

## Building

You need an `aarch64-linux-gnu-gcc` cross-compiler toolchain (or a native one, if running on ARM64).
You will also need to install the `aarch64-unknown-none-softfloat` toolchain for rust.

```shell
$ rustup target add aarch64-unknown-none-softfloat
```

```shell
$ git clone --recursive https://github.com/AsahiLinux/m1n1.git
$ cd m1n1
$ make
```

To build on a native ARM64 machine:
* On Linux, use `make ARCH=`.
* On macOS using Homebrew:
```shell
$ brew install llvm lld
$ make
```
* On macOS using MacPorts:
```shell
$ sudo port install llvm clang
$ sudo port select llvm llvm-mp-<version>
$ make
```

The output will be in `build/m1n1.macho`.

To build verbosely, use `make V=1`.

### Building using the container setup

If you have a container runtime installed, like Podman or Docker, you can make use of the compose setup, which contains all build dependencies.

```shell
$ git clone --recursive https://github.com/AsahiLinux/m1n1.git
$ cd m1n1
$ podman-compose run m1n1 make
$ # or
$ docker-compose run m1n1 make
```

## Usage

Our [wiki](https://asahilinux.org/docs/sw/m1n1-user-guide/) has more information on how to
use m1n1.

To install on an OS container based on macOS <12.1, use `m1n1.macho`:

```shell
kmutil configure-boot -c m1n1.macho -v <path to your OS volume>
```

To install on an OS container based on macOS >=12.1, use `m1n1.bin`:

```shell
kmutil configure-boot -c m1n1.bin --raw --entry-point 2048 --lowest-virtual-address 0 -v <path to your OS volume>
```

## Google GS201 / Pixel 7a ("lynx")

This tree also builds for Google's GS201 (Tensor G2). ABL loads it as an Android
boot image kernel at EL2 and passes the bootloader's own FDT in `x0`; the
platform layer lives in `src/gs201/`.

```shell
$ make PLATFORM=gs201 USE_CLANG=1
$ fastboot boot build/lynx.img
```

There is no accessible UART on this board. The live DECON framebuffer ABL
leaves armed is the local console; m1n1 exposes its proxy as a USB CDC-ACM
device before it hands off to a payload.

After `fastboot boot`, wait for the CDC device and connect from this tree:

```shell
$ cd proxyclient
$ M1N1DEVICE=/dev/ttyACM0 python3 -m m1n1.shell
```

An XNU image can be appended after m1n1's own image, at `_payload_start`.
While a payload is present, m1n1 remains in the USB proxy for inspection and
experimentation; run `p.exit()` in the proxy shell to send `P_EXIT`, leave the
proxy loop, and chainload that payload:

```shell
$ scripts/mkimage-lynx.sh build/m1n1.bin lynx-xnu.img kernel.development.vmapple
$ fastboot boot lynx-xnu.img
```

The loader accepts both `MH_EXECUTE` kernels (plain iBoot handoff: `x0` =
boot_args at EL1 with the MMU off) and `MH_FILESET` kernelcaches.
`config_sptm` builds -- `kernelcache.research.vphone600` among them -- are
detected and refused: those are entered by Apple's Secure Page Table Monitor at
GL2 and need a monitor stand-in, which this port does not provide.

Two things a plain-handoff arm64e kernel needs that are easy to get wrong:

* PAC must be *enabled* for EL1 (`HCR_EL2.API|APK`). With those bits clear,
  every PAC instruction traps to EL2 (EC 0x09) and the kernel dies at its
  first `pacibsp`; the same bits are what KVM sets in `HCR_HOST_NVHE_FLAGS`.
* The image must land on a 2 MiB boundary. The kernel's early bootstrap
  describes the image in 2 MiB units, so an unaligned physical base makes it
  build page tables that point somewhere else, and it then executes the wrong
  bytes.

With both in place the VMAPPLE kernel runs its own early boot to the point
where it trips its PAC-failure trap (`brk #0xbffd`, ESR EC 0x1c): its
statically signed pointers were signed with the key material an Apple
hypervisor hands over via `PAC_GET_DEFAULT_KEYS`, which this port cannot
reproduce. Booting further needs either that key material, the kernel's PAC
verification patched out, or PAC instructions emulated in the EL2 shim.

## Payloads

m1n1 supports running payloads by simple concatenation:

```shell
$ cat build/m1n1.macho Image.gz build/dtb/apple-j274.dtb initramfs.cpio.gz > m1n1-payload.macho
$ cat build/m1n1.bin Image.gz build/dtb/apple-j274.dtb initramfs.cpio.gz > m1n1-payload.bin
```

Supported payload file formats:

* Kernel images (or compatible). Must be compressed or last payload.
* Devicetree blobs (FDT). May be uncompressed or compressed.
* Initramfs cpio images. Must be compressed.

Supported compression formats:

* gzip
* xz

## License

m1n1 is licensed under the MIT license, as included in the [LICENSE](LICENSE) file.

* Copyright The Asahi Linux Contributors

Please see the Git history for authorship information.

Portions of m1n1 are based on mini:

* Copyright (C) 2008-2010 Hector Martin "marcan" <marcan@marcan.st>
* Copyright (C) 2008-2010 Sven Peter <sven@svenpeter.dev>
* Copyright (C) 2008-2010 Andre Heider <a.heider@gmail.com>

m1n1 embeds libfdt, which is dual [BSD](3rdparty_licenses/LICENSE.BSD-2.libfdt) and
[GPL-2](3rdparty_licenses/LICENSE.GPL-2) licensed and copyright:

* Copyright (C) 2014 David Gibson <david@gibson.dropbear.id.au>
* Copyright (C) 2018 embedded brains GmbH
* Copyright (C) 2006-2012 David Gibson, IBM Corporation.
* Copyright (C) 2012 David Gibson, IBM Corporation.
* Copyright 2012 Kim Phillips, Freescale Semiconductor.
* Copyright (C) 2016 Free Electrons
* Copyright (C) 2016 NextThing Co.

The ADT code in mini is also based on libfdt and subject to the same license.

m1n1 embeds [minlzma](https://github.com/ionescu007/minlzma), which is
[MIT](3rdparty_licenses/LICENSE.minlzma) licensed and copyright:

* Copyright (c) 2020 Alex Ionescu

m1n1 embeds a slightly modified version of [tinf](https://github.com/jibsen/tinf), which is
[ZLIB](3rdparty_licenses/LICENSE.tinf) licensed and copyright:

* Copyright (c) 2003-2019 Joergen Ibsen

m1n1 embeds portions taken from
[arm-trusted-firmware](https://github.com/ARM-software/arm-trusted-firmware), which is
[BSD](3rdparty_licenses/LICENSE.BSD-3.arm) licensed and copyright:

* Copyright (c) 2013-2020, ARM Limited and Contributors. All rights reserved.

m1n1 embeds [Doug Lea's malloc](ftp://gee.cs.oswego.edu/pub/misc/malloc.c) (dlmalloc), which is in
the public domain ([CC0](3rdparty_licenses/LICENSE.CC0)).

m1n1 embeds portions of [PDCLib](https://github.com/DevSolar/pdclib), which is in the public
domain ([CC0](3rdparty_licenses/LICENSE.CC0)).

m1n1 embeds the [Source Code Pro](https://github.com/adobe-fonts/source-code-pro) font, which is
licensed under the [OFL-1.1](3rdparty_licenses/LICENSE.OFL-1.1) license and copyright:

* Copyright 2010-2019 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'. All Rights Reserved. Source is a trademark of Adobe in the United States and/or other countries.
* This Font Software is licensed under the SIL Open Font License, Version 1.1.

m1n1 embeds portions of the [dwc3 usb linux driver](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/usb/dwc3/core.h?id=7bc5a6ba369217e0137833f5955cf0b0f08b0712), which was [BSD-or-GPLv2 dual-licensed](3rdparty_licenses/LICENSE.BSD-3.dwc3) and copyright
* Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com

m1n1 embeds portions of [musl-libc](https://musl.libc.org/)'s floating point library, which are MIT licensed and copyright
* Copyright (c) 2017-2018, Arm Limited.

m1n1 embeds some rust crates. Licenses can be found in the vendor directory for every crate.
