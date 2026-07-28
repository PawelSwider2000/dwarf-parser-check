include(FetchContent)

find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest_main)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(dwarf_parser_check_tests
  app_gtest.cpp
)

target_link_libraries(dwarf_parser_check_tests PRIVATE
  dwarf_parser_check_lib
  GTest::gtest_main
)

target_compile_definitions(dwarf_parser_check_tests PRIVATE
  DPC_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

if(DPC_CAN_BUILD_IGA_ADAPTER)
  target_compile_definitions(dwarf_parser_check_tests PRIVATE DPC_HAVE_IGA_ADAPTER=1)
endif()

if(DPC_CAN_BUILD_GDB_INTEL_ADAPTER)
  target_compile_definitions(dwarf_parser_check_tests PRIVATE DPC_HAVE_GDB_INTEL_ADAPTER=1)
endif()

gtest_discover_tests(dwarf_parser_check_tests
  PROPERTIES
    TIMEOUT 5
)

find_program(CARGO_EXECUTABLE cargo)
if(CARGO_EXECUTABLE)
  add_test(
    NAME rust_gimli_integration_tests
    COMMAND ${CARGO_EXECUTABLE} test --manifest-path ${CMAKE_SOURCE_DIR}/src/adapters/gimli/Cargo.toml --tests
  )
endif()

add_test(
  NAME dwarf_check_shell_tests
  COMMAND ${CMAKE_SOURCE_DIR}/tests/dwarf_check_test.sh
)