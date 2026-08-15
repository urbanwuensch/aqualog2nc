# CMakeLists.txt for liborigin
#
# aqualog2nc is built with vcpkg providing netCDF-cxx4 - see vcpkg.json.
# Configure with:
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=<triplet>
# where <triplet> is x64-linux, x64-osx, arm64-osx, or x64-windows-static.
# See aqualog2nc_deployment_guide.md for the full cross-platform build/release setup.

cmake_minimum_required(VERSION 3.16.3)

# Match the MSVC runtime library used by vcpkg's static triplets
# (x64-windows-static builds every dependency with /MT). Without this, our
# own targets default to /MD, which causes LNK2038 "RuntimeLibrary
# mismatch" errors when linking against those static libraries. Harmless
# no-op on Linux/macOS.
if (POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project( liborigin
    VERSION 3.0.4
    LANGUAGES CXX
)

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED TRUE )
set( CMAKE_CXX_EXTENSIONS OFF )

# allow for installation of individual targets
set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY true)

option(BUILD_SHARED_LIBS "Build shared library" ON)
option(BUILD_STATIC_LIBS "Build static library" ON)

if( NOT BUILD_STATIC_LIBS AND NOT BUILD_SHARED_LIBS )
    message( FATAL_ERROR "You must enable building shared or static library" )
endif()

if( BUILD_SHARED_LIBS AND MSVC )
    set( BUILD_STATIC_LIBS OFF CACHE BOOL "Build static library" FORCE )
endif()

if( MSVC )
    # /wd4456 /wd4458 Silent "declaration of %1 hides %2 ..."
    set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /W4 /permissive- /wd4456 /wd4458" )
    add_compile_definitions( _CRT_SECURE_NO_WARNINGS
        _SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING )
    if( CMAKE_CXX_COMPILER_ID STREQUAL "Clang" )
        set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-nonportable-include-path" )
    else()
        add_compile_options( "/MP" )
    endif()
else()
    set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -Wall -Wextra -pedantic -Wzero-as-null-pointer-constant" )
    set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic" )
endif()

# compile-time configuration variables to be linked in
configure_file(config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# source files
set (sources
    OriginFile.cpp
    OriginParser.cpp
    OriginAnyParser.cpp
)

# header files for development
set (devel-headers
    OriginObj.h
    OriginFile.h
    OriginParser.h
    tree.hh
)

# object library
add_library (origin OBJECT ${sources})
set_target_properties(origin PROPERTIES POSITION_INDEPENDENT_CODE ON)

# inform on log setting
option(GENERATE_CODE_FOR_LOG "Enable logging file parsing" OFF)
if (GENERATE_CODE_FOR_LOG)
    message("File parsing will be logged.")
    target_compile_definitions(origin PRIVATE GENERATE_CODE_FOR_LOG)
else()
    message("File parsing will NOT be logged. Define GENERATE_CODE_FOR_LOG to activate logging.")
endif ()

include(GNUInstallDirs)

# Static library
if (BUILD_STATIC_LIBS)
    add_library (origin_static STATIC $<TARGET_OBJECTS:origin>)
    set_target_properties(origin_static PROPERTIES OUTPUT_NAME "origin" )
    install( TARGETS origin_static
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif ()

# Shared library
if (BUILD_SHARED_LIBS)
    if (WIN32)
        target_compile_definitions( origin
            PUBLIC ORIGIN_DLL
            PRIVATE ORIGIN_DLL_EXPORT
        )
    endif()
    add_library( origin_shared SHARED $<TARGET_OBJECTS:origin>)
    set_target_properties(origin_shared PROPERTIES
        OUTPUT_NAME "origin"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
    install( TARGETS origin_shared
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif ()

# command line util
if (BUILD_STATIC_LIBS)
    add_executable(opj2dat opj2dat.cpp)
    target_link_libraries (opj2dat origin_static)

    add_executable(aqualog2nc aqualog2nc.cpp)

    # Portable, cross-platform netCDF C++ dependency via vcpkg - replaces
    # the old hardcoded /opt/homebrew paths (macOS-Homebrew-only, and only
    # on the machine that installed it there). See vcpkg.json for the
    # dependency declaration and the deployment guide for the full
    # per-platform build setup.
    find_package(netCDFCxx CONFIG REQUIRED)

    target_link_libraries(
        aqualog2nc
        PRIVATE
        origin_static
        netCDF::netcdf-cxx4
    )
    # Statically link libstdc++/libgcc into the Linux binary, so it
    # doesn't depend on the target system's own libstdc++.so.6 having a
    # new enough GLIBCXX symbol version. ubuntu-latest's toolchain (what
    # CI builds with) is newer than what many "stable" distros ship
    # (e.g. GLIBCXX_3.4.32, requiring roughly GCC 14) - without this, the
    # released binary fails to start on those systems with an error like
    # "version `GLIBCXX_3.4.32' not found". Only the base libc is left as
    # a dynamic dependency, which has much stronger cross-distro ABI
    # stability. No-op on macOS/Windows (MSVC's static CRT, set via
    # CMAKE_MSVC_RUNTIME_LIBRARY above, already solves the equivalent
    # problem there).
    if (UNIX AND NOT APPLE)
        target_link_options(aqualog2nc PRIVATE -static-libgcc -static-libstdc++)
    endif ()

    # aqualog2nc uses std::filesystem (folder batch mode). This is part of
    # the standard library on all reasonably current compilers; only add
    # this if your CI build reports an "undefined reference to
    # std::filesystem" link error (typically GCC < 9 on Linux):
    # target_link_libraries(aqualog2nc PRIVATE stdc++fs)


elseif (BUILD_SHARED_LIBS)
    add_executable(opj2dat opj2dat.cpp)
    target_link_libraries (opj2dat origin_shared)
endif ()

# Generate a pkg-config file matching this config
configure_file("liborigin.pc.in" "liborigin.pc" @ONLY)

# install headers
install(FILES ${devel-headers} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/liborigin)
# install pkg-config file
if (BUILD_STATIC_LIBS OR BUILD_SHARED_LIBS)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/liborigin.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
    install(TARGETS opj2dat DESTINATION ${CMAKE_INSTALL_BINDIR} OPTIONAL)
endif ()

# documentation
install(FILES COPYING FORMAT README
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    OPTIONAL
)

# Doxygen
configure_file(doc/Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
find_package(Doxygen)
if (DOXYGEN_FOUND)
    MESSAGE (STATUS "Building Doxygen docu")
    add_custom_target(doc ALL ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES ${CMAKE_CURRENT_BINARY_DIR}/html)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/html
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        OPTIONAL
    )
endif ()
