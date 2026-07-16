option(DPC_ENABLE_GIMLI_ADAPTER "Build the Rust gimli/addr2line adapter" ON)

if(NOT DPC_ENABLE_GIMLI_ADAPTER)
  return()
endif()

find_program(CARGO_EXECUTABLE cargo)
if(NOT CARGO_EXECUTABLE)
  message(WARNING "Disabling DPC_ENABLE_GIMLI_ADAPTER because cargo was not found in PATH.")
  return()
endif()

find_package(Threads REQUIRED)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(DPC_RUST_PROFILE release)
  set(DPC_RUST_BUILD_FLAG --release)
else()
  set(DPC_RUST_PROFILE debug)
  set(DPC_RUST_BUILD_FLAG)
endif()

set(DPC_RUST_CRATE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
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

target_sources(dwarf_parser_check_lib PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/rust_gimli_adapter.cpp
)

target_compile_definitions(dwarf_parser_check_lib PUBLIC DPC_HAVE_RUST_GIMLI_ADAPTER=1)

target_link_libraries(dwarf_parser_check_lib PRIVATE
  dpc_addr2line_rust
  Threads::Threads
  ${CMAKE_DL_LIBS}
  m
)
