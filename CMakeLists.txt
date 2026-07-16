cmake_minimum_required(VERSION 3.20)

project(dwarf-parser-check VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DPC_ENABLE_WARNINGS "Enable compiler warnings" ON)
option(DPC_ENABLE_DUMMY_ADAPTER "Build the synthetic dummy adapter" ON)

set(DPC_SOURCES
  src/cli.cpp
  src/core.cpp
  src/compare.cpp
)

if(DPC_ENABLE_DUMMY_ADAPTER)
  list(APPEND DPC_SOURCES
    src/adapters/dummy_adapter.cpp
  )
endif()

add_library(dwarf_parser_check_lib ${DPC_SOURCES})

add_executable(dwarf-parser-check src/main.cpp)
target_link_libraries(dwarf-parser-check PRIVATE dwarf_parser_check_lib)

target_include_directories(dwarf_parser_check_lib
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(DPC_ENABLE_DUMMY_ADAPTER)
  target_compile_definitions(dwarf_parser_check_lib PUBLIC DPC_HAVE_DUMMY_ADAPTER=1)
endif()

add_subdirectory(src/adapters/gimli)

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

include(CTest)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
