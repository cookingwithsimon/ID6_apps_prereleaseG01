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

The MHD+ Suite Exhaust Flap page runs `MhdMaps/exhaust-flap-worker-qnx`. CMake builds it from `src/collector.c` (`EXHAUST_FLAP_ONLY`), with the MOST fallback. Copy it into the MhdMaps folder next to `mhd-maps-qnx`. This fork passes it the address from `MhdMaps/gateway-ip.txt`. The packaged XML and matching image archives must be deployed together.

Road Jet (`RoadJet`, `road-jet-qnx`) and BEAMer (`BEAMer`, `beamer-qnx`) are built by CMake too, with their XML scaled to 1440×540 and the same `gateway-ip.txt` rule. Road Jet only reads the DME pedal and the EPS steering angle. BEAMer writes lamp pulses to the FEM/BDC at 0x40 in Park at zero speed, and stops with "unsupported" unless the module identifies as the F-series FEM it was tested on. It is untested on a G01.

## New firmware

Use the [car identification steps](../../AgentReadMe.md#identify-the-car), the [porting workflow](PORTING.md), and the [read-only capture tool](../tools/capture_headunit.py) to collect the platform, display, and framework evidence before building a candidate profile.

Read `/opt/sys/etc/nbt_version.txt`, `uname -a`, and `/opt/sys/bin/LayerManagerControl get screen 0` over SSH. Record the on-screen version and known iDrive pixel resolution if the corresponding automatic query fails. Compare the two framework hashes with the reference table in `AgentReadMe.md`. For a different pair, compare static ABI evidence with the already tested reference using `native/tools/audit_framework_abi.py` with `pyelftools` and `capstone`. Verify all imported symbols, constructor signatures, and vtable slots, then run a parked load and lifecycle test on that firmware. Static similarity alone does not establish runtime compatibility. Add a firmware entry only with matching evidence. Keep firmware approval separate from the vehicle's DME/EGS signal mapping.

The committed [framework fingerprint](../native/framework-fingerprint.json) lets you run this audit with only your own captured pair. It holds the symbol names, sizes, vtable relocation slots, and SHA-256 digests of the NBTevo_N18032I reference pair for the functions the seven packaged plugins import. It contains no framework code. Install the two Python packages, extract the plugin libraries from [packages/](../../packages/), and audit all of them in one run:

```sh
python3 -m pip install pyelftools capstone
python3 native/tools/audit_framework_abi.py \
  --fingerprint native/framework-fingerprint.json \
  --installed-dir /private/candidate-frameworks \
  --plugin /private/plugins/lib*.so --report /private/abi-report.json
```

The published ten-build [static comparison](COMPATIBILITY.md#static-framework-checks) used S19221I as its reference. To compare against a reference pair you hold yourself, pass its directory:

```sh
python3 native/tools/audit_framework_abi.py \
  --reference-dir /private/reference-frameworks \
  --installed-dir /private/candidate-frameworks \
  --plugin native/out/libBoostDeck.so --report /private/boostdeck-abi-report.json
```

After changing a plugin's framework imports, regenerate the fingerprint from the private reference pair with `--reference-dir`, `--plugin` for every packaged plugin, `--emit-fingerprint native/framework-fingerprint.json`, and `--label`. `native/tests/test_fingerprint.py` checks that the fingerprint covers the packaged plugin imports and expected vtables.

Inspect the report's limitations. The tool checks required imports, function instruction prefixes, and expected vtable relocation slots. It does not simulate HMI calls or prove that a matched framework works at runtime.
