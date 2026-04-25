# FindDbgEng.cmake
#
# Locate the Microsoft DbgEng headers and import libraries. These ship
# with the Windows 10 SDK, which is already on disk when MSVC is
# installed — CMake's standard Windows SDK discovery populates the
# toolchain include/link search paths, so in practice we just need to
# name the libraries to link.
#
# Defines:
#   DbgEng::DbgEng  imported interface target linking dbgeng + dbghelp

if(NOT WIN32)
    set(DbgEng_FOUND FALSE)
    return()
endif()

add_library(DbgEng::DbgEng INTERFACE IMPORTED)
target_link_libraries(DbgEng::DbgEng INTERFACE dbgeng dbghelp ole32)

set(DbgEng_FOUND TRUE)
