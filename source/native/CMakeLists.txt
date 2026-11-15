# Cross-build for NBT Evo with the bmw-tools LLVM SDK (clang + lld, no QNX SDP).
# Link against the head unit's own framework libraries:
#
#   cmake -S source/native -B /tmp/id6-build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$SDK/share/cmake/QnxSdk/toolchain.cmake \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DRHMI_LIBDIR=/path/to/opt/hmi/ID5/lib
#   cmake --build /tmp/id6-build
#
# Copies from a different NBTevo software version can prove the source compiles.
# Install only a build linked against this head unit's libAppFramework.A.so and
# libCoreFramework.A.so.
cmake_minimum_required(VERSION 3.24)
project(id6apps C CXX)
find_package(QnxSdk CONFIG REQUIRED)
set(CMAKE_SKIP_RPATH ON)

set(RHMI_LIBDIR "" CACHE PATH "Folder with libAppFramework.A.so and libCoreFramework.A.so")
foreach(lib AppFramework.A CoreFramework.A)
  if(NOT EXISTS "${RHMI_LIBDIR}/lib${lib}.so")
    message(FATAL_ERROR "Missing ${RHMI_LIBDIR}/lib${lib}.so")
  endif()
  add_library("bmw_${lib}" SHARED IMPORTED)
  set_target_properties("bmw_${lib}" PROPERTIES
    IMPORTED_LOCATION "${RHMI_LIBDIR}/lib${lib}.so"
    IMPORTED_SONAME "lib${lib}.so")
endforeach()

function(id6_plugin name soname)
  add_library(${name} SHARED ${ARGN})
  target_include_directories(${name} PRIVATE src sdk)
  target_compile_options(${name} PRIVATE -fPIC
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++98 -O2 -Wall -Wextra -fno-exceptions -fno-strict-aliasing>
    $<$<COMPILE_LANGUAGE:C>:-std=c99 -O2 -Wall -Wextra>)
  target_link_libraries(${name} PRIVATE bmw_AppFramework.A bmw_CoreFramework.A QNX::m QNX::socket)
  add_dependencies(${name} libc_plus)
  # ld.lld is invoked directly, so -Wl is not a linker flag. The toolchain rule
  # already writes --soname from the output name.
  set_target_properties(${name} PROPERTIES PREFIX "lib" OUTPUT_NAME "${name}" SUFFIX ".so")
  set_target_properties(${name} PROPERTIES LINK_FLAGS "--allow-shlib-undefined")
endfunction()

# getppid is in the car's libc and missing from the bmw-tools import list.
# Rebuild the link stub with that symbol and the same SONAME, and use it
# instead of the SDK libc so the two copies are not both named libc.so.3.
get_filename_component(LLVM_BIN "${CMAKE_AR}" DIRECTORY)
set(LIBC_PLUS "${CMAKE_BINARY_DIR}/libc-plus.so")
string(REGEX MATCH "[^ ]*libc\\.so\\.3" SDK_LIBC "${CMAKE_C_STANDARD_LIBRARIES}")
if(NOT SDK_LIBC)
  message(FATAL_ERROR "libc.so.3 is not in CMAKE_C_STANDARD_LIBRARIES")
endif()
add_custom_command(
  OUTPUT "${LIBC_PLUS}"
  COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/extend_libc_stub.py"
          "${CMAKE_C_COMPILER}" "${LLVM_BIN}/llvm-readelf" "${CMAKE_LINKER}"
          "${SDK_LIBC}" "${LIBC_PLUS}" "${CMAKE_BINARY_DIR}/libc-plus.s"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/extend_libc_stub.py"
  VERBATIM)
add_custom_target(libc_plus DEPENDS "${LIBC_PLUS}")
string(REPLACE "${SDK_LIBC}" "${LIBC_PLUS}" CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES}")
string(REPLACE "${SDK_LIBC}" "${LIBC_PLUS}" CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES}")

function(id6_worker name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE src)
  target_compile_options(${name} PRIVATE -std=c99 -O2 -Wall -Wextra)
  add_dependencies(${name} libc_plus)
  target_link_libraries(${name} PRIVATE QNX::m QNX::socket)
endfunction()

id6_plugin(BoostDeck libBoostDeck.so src/plugin.cpp src/gauge_core.c)
target_compile_definitions(BoostDeck PRIVATE PERF_LAB)
id6_worker(boostdeck-collector-qnx src/collector.c src/gauge_core.c)
target_compile_definitions(boostdeck-collector-qnx PRIVATE NATIVE_FAST PERF_LAB)

id6_plugin(ShiftDeck libShiftDeck.so src/shift_deck.cpp src/gauge_core.c)
target_compile_definitions(ShiftDeck PRIVATE PERF_LAB NATIVE_SHIFT_SMOOTH)
id6_worker(shift-deck-qnx src/collector.c src/gauge_core.c)
target_compile_definitions(shift-deck-qnx PRIVATE NATIVE_FAST SHIFT_DECK_ONLY)

id6_plugin(MhdMaps libMhdMaps.so src/mhd_maps.cpp)
target_compile_definitions(MhdMaps PRIVATE PERF_LAB)
id6_worker(mhd-maps-qnx src/collector.c src/gauge_core.c)
target_compile_definitions(mhd-maps-qnx PRIVATE MHD_MAPS_ONLY)

id6_plugin(EngineCodes libEngineCodes.so src/engine_codes.cpp src/gauge_core.c)
target_compile_definitions(EngineCodes PRIVATE PERF_LAB)
id6_worker(engine-codes-qnx src/collector.c src/gauge_core.c)
target_compile_definitions(engine-codes-qnx PRIVATE ENGINE_CODES_ONLY)

id6_worker(gaugedeck-fileops fileops.c)
