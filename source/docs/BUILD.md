# Build and deploy

Run commands from `source/`. Host tests use a C compiler, Make, and Python 3. No car or BMW files are needed for these tests.

```sh
make -C native CC=gcc test
python3 tools/test_probe_signals.py
```

## Native payload

This fork builds with the MIT [bmw-tools](https://github.com/agent00jackson/bmw-tools) LLVM SDK for QNX 6.5 ARMv7 (clang and lld). `native/build.sh` still calls `qcc` from a QNX SDP install. Use the CMake build below.

The SDK libc import list omits `getppid`, `lstat`, and `rmdir`, and, for the MOST fallback, `alarm`, `sigprocmask`, `vfprintf`, `pthread_condattr_init`, `pthread_condattr_setclock`, and `pthread_condattr_destroy`. CMake generates a link-only `libc.so.3` with those symbols added and the same SONAME, so the head-unit binary imports them from the car's libc. Do not copy that generated stub onto the car. Do not copy the SDK's `usr/lib/*.so` stubs onto the car either. They are trap libraries for the linker.

1. Install the bmw-tools QNX SDK and note `share/cmake/QnxSdk/toolchain.cmake`.

2. Copy this head unit's framework libraries. They are the link inputs for an install build. Use a binary-safe transfer, and pass the legacy SSH algorithms only for that ENET connection.

```sh
mkdir -p native/frameworks
ssh root@HEAD_IP 'cat /opt/hmi/ID5/lib/libAppFramework.A.so' > native/frameworks/libAppFramework.A.so
ssh root@HEAD_IP 'cat /opt/hmi/ID5/lib/libCoreFramework.A.so' > native/frameworks/libCoreFramework.A.so
shasum -a 256 native/frameworks/*.so
```

On a split-node unit (`uname -a` reports `hu-jacinto`), `/opt/hmi/ID5/lib` is empty on the SSH node; read the libraries over Qnet under `/net/hu-omap/fs/sda0/...` instead (see [Split-node units](PORTING.md#split-node-units)).

3. Configure and build on the Linux filesystem. `RHMI_LIBDIR` must be the directory from the step above when the binaries are meant for the car. Put the build directory on the Linux filesystem, not under a Windows mount.

```sh
cmake -S native -B "$HOME/id6-build" -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$SDK/share/cmake/QnxSdk/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DRHMI_LIBDIR="$PWD/native/frameworks"
cmake --build "$HOME/id6-build"
```

On a host where `cc` is not installed, run the tests with `make -C native CC=gcc test`.

Every worker includes the [MOST fallback](MOST.md). It opens the DSI library at run time, so no private library is needed to build it.

The XML already in `native/plugin` is the 1440×540 layout. Do not run `native/tools/make_bars.py` or `native/tools/make_drive_apps.py` over those files. The image archives stay as they are; the HMI scales them to the width and height in the XML. Place `gateway-ip.txt` in each installed app directory, containing one IPv4 address and nothing else.

## New firmware

Use the [unlisted firmware runbook](../../AgentReadMe.md#bring-up-an-unlisted-firmware) and [read-only capture tool](../tools/capture_headunit.py) to collect the platform, display, and framework evidence before building a candidate profile.

Read `/opt/sys/etc/nbt_version.txt`, `uname -a`, and `/opt/sys/bin/LayerManagerControl get screen 0` over SSH. Record the on-screen version and known iDrive pixel resolution if the corresponding automatic query fails. Compare the two framework hashes with the reference table in `AgentReadMe.md`. For a different pair, compare static ABI evidence with the already tested reference using `native/tools/audit_framework_abi.py` with `pyelftools` and `capstone`. Verify all imported symbols, constructor signatures, and vtable slots, then run a parked load and lifecycle test on that firmware. Static similarity alone does not establish runtime compatibility. Add a firmware entry only with matching evidence. Keep firmware approval separate from the vehicle's DME/EGS signal mapping.

For example, compare the candidate against an independently preserved reference library pair:

```sh
python3 native/tools/audit_framework_abi.py \
  --reference-dir /private/reference-frameworks \
  --installed-dir /private/candidate-frameworks \
  --plugin native/out/libBoostDeck.so --report /private/boostdeck-abi-report.json
```

Repeat for ShiftDeck and MHD Maps and inspect the report's limitations. The tool compares selected instruction prefixes and vtable relocation slots. It does not simulate HMI calls or prove that a matched framework works at runtime.
