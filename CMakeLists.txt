cmake_minimum_required(VERSION 3.20)

project(dwarf-parser-check VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DPC_ENABLE_WARNINGS "Enable compiler warnings" ON)

add_executable(dwarf-parser-check
  src/main.cpp
  src/cli.cpp
  src/core.cpp
  src/compare.cpp
  src/adapters/dummy_adapter.cpp
)

target_include_directories(dwarf-parser-check
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(DPC_ENABLE_WARNINGS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dwarf-parser-check PRIVATE
      -Wall
      -Wextra
      -Wpedantic
    )
  elseif(MSVC)
    target_compile_options(dwarf-parser-check PRIVATE /W4)
  endif()
endif()

include(CTest)
