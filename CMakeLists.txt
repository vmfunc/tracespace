cmake_minimum_required(VERSION 3.24)
project(tracespace LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(TRACESPACE_BUILD_TESTS "Build tests" ON)
option(TRACESPACE_BUILD_TUI "Build the FTXUI front end" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- tree-sitter core + grammars (built from sources; their own build files are skipped)
FetchContent_Declare(tree_sitter
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
  GIT_TAG v0.25.10
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_Declare(tree_sitter_c
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
  GIT_TAG v0.24.1
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_Declare(tree_sitter_cpp
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
  GIT_TAG v0.23.4
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tree_sitter tree_sitter_c tree_sitter_cpp nlohmann_json)

add_library(ts_runtime STATIC ${tree_sitter_SOURCE_DIR}/lib/src/lib.c)
target_include_directories(ts_runtime
  PUBLIC ${tree_sitter_SOURCE_DIR}/lib/include
  PRIVATE ${tree_sitter_SOURCE_DIR}/lib/src)

add_library(ts_grammars STATIC
  ${tree_sitter_c_SOURCE_DIR}/src/parser.c
  ${tree_sitter_cpp_SOURCE_DIR}/src/parser.c
  ${tree_sitter_cpp_SOURCE_DIR}/src/scanner.c)
target_include_directories(ts_grammars PRIVATE
  ${tree_sitter_c_SOURCE_DIR}/src ${tree_sitter_cpp_SOURCE_DIR}/src)
if(NOT MSVC)
  target_compile_options(ts_runtime PRIVATE -w)
  target_compile_options(ts_grammars PRIVATE -w)
endif()

# --- core library: parser, model, layout, painter, session (no FTXUI),
# plus platform I/O: file watchers and the editor socket
find_package(Threads REQUIRED)
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp src/io/*.cpp)
add_library(tracespace_core STATIC ${CORE_SOURCES})
target_include_directories(tracespace_core PUBLIC src)
target_link_libraries(tracespace_core
  PUBLIC nlohmann_json::nlohmann_json Threads::Threads
  PRIVATE ts_runtime ts_grammars)
if(MSVC)
  target_compile_options(tracespace_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(tracespace_core PRIVATE -Wall -Wextra)
endif()

# --- executable
file(GLOB APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp)
add_executable(tracespace ${APP_SOURCES})
target_link_libraries(tracespace PRIVATE tracespace_core)

if(TRACESPACE_BUILD_TUI)
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v6.1.9
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(ftxui)
  file(GLOB TUI_SOURCES CONFIGURE_DEPENDS src/tui/*.cpp)
  if(TUI_SOURCES)
    target_sources(tracespace PRIVATE ${TUI_SOURCES})
  endif()
  target_compile_definitions(tracespace PRIVATE TRACESPACE_TUI=1)
  target_link_libraries(tracespace PRIVATE ftxui::component ftxui::dom ftxui::screen)
endif()

if(WIN32)
  target_link_libraries(tracespace PRIVATE ws2_32)
  if(MINGW)
    target_link_options(tracespace PRIVATE -static)
  endif()
endif()

# --- install: the binary and the Neovim plugin (see the Makefile)
#   cmake --install build --prefix <dir> --component tracespace
include(GNUInstallDirs)
install(TARGETS tracespace RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tracespace)
install(DIRECTORY nvim/ DESTINATION ${CMAKE_INSTALL_DATADIR}/tracespace/nvim COMPONENT tracespace)

# --- tests
if(TRACESPACE_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
  file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(tracespace_tests ${TEST_SOURCES})
  target_link_libraries(tracespace_tests PRIVATE tracespace_core Catch2::Catch2WithMain)
  target_compile_definitions(tracespace_tests PRIVATE
    TRACESPACE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if(MINGW)
    target_link_options(tracespace_tests PRIVATE -static)
  endif()
  enable_testing()
  add_test(NAME tracespace_tests COMMAND tracespace_tests)
  if(TRACESPACE_BUILD_TUI)
    # the FTXUI frame, rendered without a terminal
    add_test(NAME tui_frame COMMAND tracespace --theme rounded --frame 60x12
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/inputs/example.cpp)
    set_tests_properties(tui_frame PROPERTIES
      PASS_REGULAR_EXPRESSION "funcA.*example.cpp.*execution.*rounded.*watching file")
  endif()
endif()
