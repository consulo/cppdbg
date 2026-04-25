cmake_minimum_required(VERSION 3.24)
project(cppdbg CXX)

if(NOT WIN32)
    message(FATAL_ERROR "cppdbg is Windows-only (depends on DbgEng).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Statically link the MSVC runtime so the produced .exe has no external
# runtime dependency beyond what's already on Windows.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# cppdap vendored as a submodule.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cppdap/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/cppdap is missing. Run: "
        "git submodule update --init --recursive")
endif()
set(CPPDAP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CPPDAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/cppdap)

find_package(DbgEng REQUIRED)

add_executable(cppdbg
    src/main.cpp
    src/engine.cpp
    src/event_callbacks.cpp
    src/handlers/breakpoints.cpp
    src/handlers/inspection.cpp
    src/handlers/lifecycle.cpp
    src/handlers/stepping.cpp
    src/util/wstring.cpp
)

target_include_directories(cppdbg PRIVATE src)
target_link_libraries(cppdbg PRIVATE cppdap DbgEng::DbgEng)

target_compile_definitions(cppdbg PRIVATE
    UNICODE _UNICODE
    NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(cppdbg PRIVATE /W4 /permissive-)
endif()
