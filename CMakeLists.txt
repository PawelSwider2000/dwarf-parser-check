cmake_minimum_required(VERSION 3.20)

project(dwarf-parser-check VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DPC_ENABLE_WARNINGS "Enable compiler warnings" ON)
option(DPC_ENABLE_GIMLI_ADAPTER "Build the Rust gimli/addr2line adapter" ON)

find_program(CARGO_EXECUTABLE cargo
  HINTS
    $ENV{HOME}/.cargo/bin
)

set(DPC_CAN_BUILD_GIMLI_ADAPTER OFF)
if(DPC_ENABLE_GIMLI_ADAPTER)
  if(CARGO_EXECUTABLE)
    set(DPC_CAN_BUILD_GIMLI_ADAPTER ON)
  else()
    message(WARNING "Disabling DPC_ENABLE_GIMLI_ADAPTER because cargo was not found in PATH.")
  endif()
endif()

set(DPC_LIBRARY_SOURCES
  src/cli.cpp
  src/core.cpp
  src/compare.cpp
  src/kernel_debug_manifest.cpp
)

if(DPC_CAN_BUILD_GIMLI_ADAPTER)
  list(APPEND DPC_LIBRARY_SOURCES
    src/adapters/gimli/rust_gimli_adapter.cpp
  )
endif()

add_library(dwarf_parser_check_lib STATIC ${DPC_LIBRARY_SOURCES})

target_include_directories(dwarf_parser_check_lib
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(DPC_CAN_BUILD_GIMLI_ADAPTER)
  target_compile_definitions(dwarf_parser_check_lib PRIVATE DPC_HAVE_RUST_GIMLI_ADAPTER=1)
endif()

if(DPC_CAN_BUILD_GIMLI_ADAPTER)
  find_package(Threads REQUIRED)

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(DPC_RUST_PROFILE release)
    set(DPC_RUST_BUILD_FLAG --release)
  else()
    set(DPC_RUST_PROFILE debug)
    set(DPC_RUST_BUILD_FLAG)
  endif()

  set(DPC_RUST_CRATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/gimli)
  set(DPC_RUST_TARGET_DIR ${DPC_RUST_CRATE_DIR}/target)
  set(DPC_RUST_LIBRARY ${DPC_RUST_TARGET_DIR}/${DPC_RUST_PROFILE}/libdpc_addr2line.a)

  add_custom_command(
    OUTPUT ${DPC_RUST_LIBRARY}
    COMMAND ${CARGO_EXECUTABLE} build ${DPC_RUST_BUILD_FLAG}
    WORKING_DIRECTORY ${DPC_RUST_CRATE_DIR}
    DEPENDS
      ${DPC_RUST_CRATE_DIR}/Cargo.toml
      ${DPC_RUST_CRATE_DIR}/src/lib.rs
    COMMENT "Building Rust addr2line adapter"
    VERBATIM
  )

  add_custom_target(dpc_addr2line_rust_build DEPENDS ${DPC_RUST_LIBRARY})

  add_library(dpc_addr2line_rust STATIC IMPORTED GLOBAL)
  set_target_properties(dpc_addr2line_rust PROPERTIES
    IMPORTED_LOCATION ${DPC_RUST_LIBRARY}
  )
  add_dependencies(dpc_addr2line_rust dpc_addr2line_rust_build)

  target_link_libraries(dwarf_parser_check_lib PRIVATE
    dpc_addr2line_rust
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
  )
endif()

if(DPC_ENABLE_WARNINGS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dwarf_parser_check_lib PRIVATE
      -Wall
      -Wextra
      -Wpedantic
    )
  elseif(MSVC)
    target_compile_options(dwarf_parser_check_lib PRIVATE /W4)
  endif()
endif()

add_executable(dwarf-parser-check src/main.cpp)
target_link_libraries(dwarf-parser-check PRIVATE dwarf_parser_check_lib)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
