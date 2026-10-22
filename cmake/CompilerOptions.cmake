# Warning, optimization, sanitizer and PGO flags shared by every Ironbook target.

find_package(Threads REQUIRED)

set(IRONBOOK_WARNINGS
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
  -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)

if(IRONBOOK_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
  if(_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(WARNING "LTO requested but unsupported: ${_ipo_msg}")
  endif()
endif()

# Host CPU tuning flag. On Apple silicon `-mcpu=native` must be resolved by a *native*
# compiler process: a clang launched from Rosetta-translated build tools resolves
# "native" to an x86 CPU. Ask an arm64 clang which CPU it sees and pin it explicitly.
set(IRONBOOK_NATIVE_FLAG -march=native)
if(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  file(WRITE ${CMAKE_BINARY_DIR}/ib_cpu_probe.cpp "int main() { return 0; }\n")
  execute_process(
    COMMAND arch -arm64 ${CMAKE_CXX_COMPILER} -arch arm64 -mcpu=native "-###" -c
            ${CMAKE_BINARY_DIR}/ib_cpu_probe.cpp
    ERROR_VARIABLE _ib_cc1 OUTPUT_QUIET)
  if(_ib_cc1 MATCHES "\"-target-cpu\" \"([^\"]+)\"")
    set(IRONBOOK_NATIVE_FLAG -mcpu=${CMAKE_MATCH_1})
  else()
    set(IRONBOOK_NATIVE_FLAG -mcpu=apple-m1)
  endif()
  message(STATUS "Ironbook: Apple silicon native tuning ${IRONBOOK_NATIVE_FLAG}")
endif()

function(ironbook_configure_target target)
  target_compile_options(${target} PRIVATE ${IRONBOOK_WARNINGS})

  # Release: maximum optimization for the host. Frame pointers stay on so that
  # `perf --call-graph=fp` and macOS `sample` produce usable stacks for flamegraphs.
  target_compile_options(${target} PRIVATE
    $<$<CONFIG:Release,RelWithDebInfo>:-O3 -fno-omit-frame-pointer>
    $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>)
  if(IRONBOOK_NATIVE)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:${IRONBOOK_NATIVE_FLAG}>)
  endif()

  if(IRONBOOK_SANITIZE)
    string(REPLACE ";" "," _san "${IRONBOOK_SANITIZE}")
    target_compile_options(${target} PRIVATE -fsanitize=${_san} -fno-sanitize-recover=all
                                             -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=${_san})
  endif()

  if(IRONBOOK_PGO STREQUAL "generate")
    target_compile_options(${target} PRIVATE -fprofile-instr-generate)
    target_link_options(${target} PRIVATE -fprofile-instr-generate)
  elseif(IRONBOOK_PGO)
    target_compile_options(${target} PRIVATE -fprofile-instr-use=${IRONBOOK_PGO}
                                             -Wno-profile-instr-unprofiled
                                             -Wno-profile-instr-out-of-date)
  endif()
endfunction()
