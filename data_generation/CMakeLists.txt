cmake_minimum_required(VERSION 3.20)

project(dwarf_parser_check_workload LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DPC_WORKLOAD "gemm" CACHE STRING "Workload to build")
set_property(CACHE DPC_WORKLOAD PROPERTY STRINGS
  gemm control_flow multifile inline_chain multifile_inline_chain
)
set(DPC_WORKLOAD_DEBUG "g" CACHE STRING "Device debug information: g, gline, or none")
set_property(CACHE DPC_WORKLOAD_DEBUG PROPERTY STRINGS g gline none)
set(DPC_WORKLOAD_OPT "O0" CACHE STRING "Optimization level: O0, O1, or O2")
set_property(CACHE DPC_WORKLOAD_OPT PROPERTY STRINGS O0 O1 O2)
set(DPC_WORKLOAD_DEVICE_CODE "jit" CACHE STRING "Device compilation mode: jit or aot")
set_property(CACHE DPC_WORKLOAD_DEVICE_CODE PROPERTY STRINGS jit aot)
set(DPC_WORKLOAD_AOT_TARGET "bmg,pvc" CACHE STRING "Comma-separated Intel GPU AOT targets: bmg and/or pvc")
set_property(CACHE DPC_WORKLOAD_AOT_TARGET PROPERTY STRINGS bmg,pvc bmg pvc)

if(DPC_WORKLOAD STREQUAL "gemm")
  set(DPC_WORKLOAD_SOURCES workloads/gemm/gemm_workload.cpp)
elseif(DPC_WORKLOAD STREQUAL "control_flow")
  set(DPC_WORKLOAD_SOURCES workloads/control_flow/control_flow_workload.cpp)
elseif(DPC_WORKLOAD STREQUAL "multifile")
  set(DPC_WORKLOAD_SOURCES
    workloads/multifile/multifile_workload.cpp
    workloads/multifile/multifile_helpers.cpp
    workloads/multifile/multifile_extra.cpp
  )
elseif(DPC_WORKLOAD STREQUAL "inline_chain")
  set(DPC_WORKLOAD_SOURCES workloads/inline_chain/inline_chain_workload.cpp)
elseif(DPC_WORKLOAD STREQUAL "multifile_inline_chain")
  set(DPC_WORKLOAD_SOURCES
    workloads/multifile_inline_chain/multifile_inline_chain_workload.cpp
    workloads/multifile_inline_chain/multifile_inline_chain_helpers.cpp
  )
else()
  message(FATAL_ERROR "DPC_WORKLOAD must name a supported workload")
endif()

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

add_executable(${DPC_WORKLOAD}
  main.cpp
  ${DPC_WORKLOAD_SOURCES}
  workload_utils.cpp
  kernel_debug_info.cpp
)

target_include_directories(${DPC_WORKLOAD} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(${DPC_WORKLOAD} PRIVATE
  -fsycl
  -${DPC_WORKLOAD_OPT}
)
target_link_options(${DPC_WORKLOAD} PRIVATE -fsycl)

if(DPC_WORKLOAD_DEVICE_CODE STREQUAL "aot")
  target_compile_options(${DPC_WORKLOAD} PRIVATE
    -fsycl-targets=spir64_gen
    -Xsycl-target-backend=spir64_gen
    "-device ${DPC_WORKLOAD_AOT_TARGET}"
  )
  target_link_options(${DPC_WORKLOAD} PRIVATE
    -fsycl-targets=spir64_gen
    -Xsycl-target-backend=spir64_gen
    "-device ${DPC_WORKLOAD_AOT_TARGET}"
  )
endif()

if(DPC_WORKLOAD_DEBUG STREQUAL "g")
  target_compile_options(${DPC_WORKLOAD} PRIVATE -g)
elseif(DPC_WORKLOAD_DEBUG STREQUAL "gline")
  target_compile_options(${DPC_WORKLOAD} PRIVATE -gline-tables-only)
endif()

target_link_libraries(${DPC_WORKLOAD} PRIVATE ${DPC_LEVEL_ZERO_LOADER_LIBRARY})
set_target_properties(${DPC_WORKLOAD} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)