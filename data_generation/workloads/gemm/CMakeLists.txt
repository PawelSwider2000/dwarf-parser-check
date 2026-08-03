cmake_minimum_required(VERSION 3.20)

project(dwarf_parser_check_gemm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DPC_WORKLOAD_DEBUG "g" CACHE STRING "Device debug information: g, gline, or none")
set_property(CACHE DPC_WORKLOAD_DEBUG PROPERTY STRINGS g gline none)
set(DPC_WORKLOAD_OPT "O0" CACHE STRING "Optimization level: O0, O1, or O2")
set_property(CACHE DPC_WORKLOAD_OPT PROPERTY STRINGS O0 O1 O2)
set(DPC_WORKLOAD_DEVICE_CODE "jit" CACHE STRING "Device compilation mode: jit or aot")
set_property(CACHE DPC_WORKLOAD_DEVICE_CODE PROPERTY STRINGS jit aot)
set(DPC_WORKLOAD_AOT_TARGET "bmg,pvc" CACHE STRING "Comma-separated Intel GPU AOT targets: bmg and/or pvc")
set_property(CACHE DPC_WORKLOAD_AOT_TARGET PROPERTY STRINGS bmg,pvc bmg pvc)

set(_dpc_workload_debug_options g gline none)
if(NOT DPC_WORKLOAD_DEBUG IN_LIST _dpc_workload_debug_options)
  message(FATAL_ERROR "DPC_WORKLOAD_DEBUG must be g, gline, or none")
endif()

set(_dpc_workload_opt_options O0 O1 O2)
if(NOT DPC_WORKLOAD_OPT IN_LIST _dpc_workload_opt_options)
  message(FATAL_ERROR "DPC_WORKLOAD_OPT must be O0, O1, or O2")
endif()

set(_dpc_workload_device_code_options jit aot)
if(NOT DPC_WORKLOAD_DEVICE_CODE IN_LIST _dpc_workload_device_code_options)
  message(FATAL_ERROR "DPC_WORKLOAD_DEVICE_CODE must be jit or aot")
endif()

set(_dpc_workload_aot_target_options bmg pvc)
string(REPLACE "," ";" _dpc_workload_aot_targets "${DPC_WORKLOAD_AOT_TARGET}")
foreach(_dpc_workload_aot_target IN LISTS _dpc_workload_aot_targets)
  if(NOT _dpc_workload_aot_target IN_LIST _dpc_workload_aot_target_options)
    message(FATAL_ERROR "DPC_WORKLOAD_AOT_TARGET must contain only bmg and/or pvc")
  endif()
endforeach()

find_library(DPC_LEVEL_ZERO_LOADER_LIBRARY NAMES ze_loader REQUIRED)

add_executable(gemm
  gemm_main.cpp
  ../../kernel_debug_info.cpp
)

target_include_directories(gemm PRIVATE ../..)
target_compile_options(gemm PRIVATE
  -fsycl
  -${DPC_WORKLOAD_OPT}
)
target_link_options(gemm PRIVATE -fsycl)

if(DPC_WORKLOAD_DEVICE_CODE STREQUAL "aot")
  target_compile_options(gemm PRIVATE
    -fsycl-targets=spir64_gen
    -Xsycl-target-backend=spir64_gen
    "-device ${DPC_WORKLOAD_AOT_TARGET}"
  )
  target_link_options(gemm PRIVATE
    -fsycl-targets=spir64_gen
    -Xsycl-target-backend=spir64_gen
    "-device ${DPC_WORKLOAD_AOT_TARGET}"
  )
endif()

if(DPC_WORKLOAD_DEBUG STREQUAL "g")
  target_compile_options(gemm PRIVATE -g)
elseif(DPC_WORKLOAD_DEBUG STREQUAL "gline")
  target_compile_options(gemm PRIVATE -gline-tables-only)
endif()

target_link_libraries(gemm PRIVATE ${DPC_LEVEL_ZERO_LOADER_LIBRARY})
set_target_properties(gemm PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)