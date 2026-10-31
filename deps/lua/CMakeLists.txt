# CMakeLists.txt for Lua
# Cross-platform build system
# Supports all platforms from original Makefile: guess aix bsd c89 freebsd generic ios linux macosx mingw posix solaris

cmake_minimum_required(VERSION 3.12...3.28)

# Project definition with semantic versioning
project(lua
        VERSION 5.5.0
        DESCRIPTION "Lua programming language"
        HOMEPAGE_URL "https://www.lua.org"
        LANGUAGES C
)

# Only set policies if they exist
foreach (policy CMP0077 CMP0075)
    if (POLICY ${policy})
        cmake_policy(SET ${policy} NEW)
    endif ()
endforeach ()

# Include standard modules
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckIncludeFile)
include(CheckLibraryExists)
include(CheckFunctionExists)
include(CheckCSourceCompiles)

#==============================================================================
# Build Options
#==============================================================================

option(LUA_BUILD_SHARED "Build shared library" OFF)
option(LUA_BUILD_STATIC "Build static library" ON)
option(LUA_BUILD_READLINE "Build with readline support (Unix only)" OFF)
option(LUA_BUILD_LIBEDIT "Build with libedit support (alternative to readline)" OFF)
option(LUA_BUILD_TESTS "Build test suite" OFF)
option(LUA_ENABLE_LTO "Enable Link Time Optimization" OFF)
option(LUA_FORCE_C89 "Force C89 standard compliance" OFF)
option(LUA_VERSIONED_INSTALL "Add version suffix to binaries and libraries" ON)

# Validate options
if (NOT LUA_BUILD_SHARED AND NOT LUA_BUILD_STATIC)
    message(FATAL_ERROR "At least one of LUA_BUILD_SHARED or LUA_BUILD_STATIC must be enabled")
endif ()

#==============================================================================
# Version Configuration
#==============================================================================

# Generate version suffixes
if (LUA_VERSIONED_INSTALL)
    set(LUA_VERSION_SUFFIX_DOT "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
    set(LUA_VERSION_SUFFIX_COMPACT "${PROJECT_VERSION_MAJOR}${PROJECT_VERSION_MINOR}")
    message(STATUS "Version suffixes enabled: ${LUA_VERSION_SUFFIX_DOT}, ${LUA_VERSION_SUFFIX_COMPACT}")
else ()
    set(LUA_VERSION_SUFFIX_DOT "")
    set(LUA_VERSION_SUFFIX_COMPACT "")
endif ()

#==============================================================================
# Platform Detection and Configuration
#==============================================================================

set(LUA_PLATFORM "guess" CACHE STRING "Target platform")
set_property(CACHE LUA_PLATFORM PROPERTY STRINGS
        "guess;aix;bsd;c89;freebsd;generic;ios;linux;macosx;mingw;posix;solaris")

# Platform auto-detection
if (LUA_PLATFORM STREQUAL "guess")
    if (CMAKE_SYSTEM_NAME MATCHES "Linux")
        set(LUA_PLATFORM "linux")
    elseif (CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set(LUA_PLATFORM $<IF:$<BOOL:${CMAKE_OSX_SYSROOT}>,"ios","macosx">)
        if (CMAKE_OSX_SYSROOT MATCHES "iphone")
            set(LUA_PLATFORM "ios")
        else ()
            set(LUA_PLATFORM "macosx")
        endif ()
    elseif (CMAKE_SYSTEM_NAME MATCHES "Windows")
        set(LUA_PLATFORM $<IF:$<BOOL:${MINGW}>,"mingw","generic">)
        if (MINGW)
            set(LUA_PLATFORM "mingw")
        else ()
            set(LUA_PLATFORM "generic")
        endif ()
    elseif (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
        set(LUA_PLATFORM "freebsd")
    elseif (CMAKE_SYSTEM_NAME MATCHES "(OpenBSD|NetBSD)")
        set(LUA_PLATFORM "bsd")
    elseif (CMAKE_SYSTEM_NAME MATCHES "AIX")
        set(LUA_PLATFORM "aix")
    elseif (CMAKE_SYSTEM_NAME MATCHES "SunOS")
        set(LUA_PLATFORM "solaris")
    elseif (UNIX)
        set(LUA_PLATFORM "posix")
    else ()
        set(LUA_PLATFORM "generic")
    endif ()
endif ()

message(STATUS "Target platform: ${LUA_PLATFORM}")

#==============================================================================
# Compiler Configuration
#==============================================================================

# C standard
if (LUA_FORCE_C89 OR LUA_PLATFORM STREQUAL "c89")
    set(CMAKE_C_STANDARD 90)
else ()
    set(CMAKE_C_STANDARD 99)
endif ()
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Compiler-specific flags
set(LUA_COMPILE_OPTIONS "")
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    list(APPEND LUA_COMPILE_OPTIONS
            $<$<CONFIG:Debug>:-Wall -Wextra -Wdeclaration-after-statement>
            $<$<AND:$<BOOL:${LUA_ENABLE_LTO}>,$<CONFIG:Release>>:-flto>
    )
elseif (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    list(APPEND LUA_COMPILE_OPTIONS
            $<$<CONFIG:Debug>:/W3>
            $<$<AND:$<BOOL:${LUA_ENABLE_LTO}>,$<CONFIG:Release>>:/GL>
    )
    if (LUA_ENABLE_LTO)
        set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
    endif ()
elseif (CMAKE_C_COMPILER_ID STREQUAL "Intel")
    list(APPEND LUA_COMPILE_OPTIONS $<$<CONFIG:Debug>:-Wall>)
endif ()

#==============================================================================
# System Capability Detection
#==============================================================================

# Check for headers and libraries
check_include_file("unistd.h" HAVE_UNISTD_H)
check_include_file("readline/readline.h" HAVE_READLINE_H)
check_include_file("editline/readline.h" HAVE_EDITLINE_H)
check_include_file("dlfcn.h" HAVE_DLFCN_H)

check_library_exists(m sin "" HAVE_LIBM)
check_library_exists(dl dlopen "" HAVE_LIBDL)
check_library_exists(readline readline "" HAVE_LIBREADLINE)
check_library_exists(edit readline "" HAVE_LIBEDIT)

#==============================================================================
# Platform-Specific Configuration
#==============================================================================

# Initialize platform-specific variables
set(LUA_COMPILE_DEFINITIONS "")
set(LUA_LINK_LIBRARIES "")

# Platform-specific settings. MSVC's CRT already provides the math routines;
# a separate Unix-style libm does not exist on Windows.
if (WIN32)
    # Lua's headers select the Windows implementation from _WIN32.

elseif (LUA_PLATFORM STREQUAL "linux")
    list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_LINUX)
    list(APPEND LUA_LINK_LIBRARIES $<$<BOOL:${HAVE_LIBDL}>:dl> $<$<BOOL:${HAVE_LIBM}>:m>)

elseif (LUA_PLATFORM STREQUAL "macosx")
    list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_MACOSX)
    list(APPEND LUA_LINK_LIBRARIES $<$<BOOL:${HAVE_LIBM}>:m>)

elseif (LUA_PLATFORM STREQUAL "ios")
    list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_IOS)
    list(APPEND LUA_LINK_LIBRARIES $<$<BOOL:${HAVE_LIBM}>:m>)

elseif (LUA_PLATFORM STREQUAL "freebsd")
    list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_LINUX)
    list(APPEND LUA_LINK_LIBRARIES $<$<BOOL:${HAVE_LIBM}>:m>)

elseif (LUA_PLATFORM STREQUAL "bsd")
    list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_POSIX)
    list(APPEND LUA_LINK_LIBRARIES $<$<BOOL:${HAVE_LIBM}>:m>)

elseif (LUA_PLATFORM STREQUAL "aix")
    list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_POSIX _ALL_SOURCE)
    list(APPEND LUA_LINK_LIBRARIES $<$<BOOL:${HAVE_LIBDL}>:dl> $<$<BOOL:${HAVE_LIBM}>:m>)

elseif (LUA_PLATFORM STREQUAL "solaris")
    list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_POSIX)
    list(APPEND LUA_LINK_LIBRARIES $<$<BOOL:${HAVE_LIBDL}>:dl> $<$<BOOL:${HAVE_LIBM}>:m> socket nsl)

elseif (LUA_PLATFORM STREQUAL "posix")
    list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_POSIX)
    list(APPEND LUA_LINK_LIBRARIES $<$<BOOL:${HAVE_LIBDL}>:dl> $<$<BOOL:${HAVE_LIBM}>:m>)

elseif (LUA_PLATFORM STREQUAL "c89")
    list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_C89)
    list(APPEND LUA_LINK_LIBRARIES $<$<BOOL:${HAVE_LIBM}>:m>)

else () # generic or mingw
    list(APPEND LUA_LINK_LIBRARIES $<$<BOOL:${HAVE_LIBM}>:m>)
endif ()

# Readline/Libedit support
if (LUA_BUILD_READLINE AND NOT LUA_PLATFORM MATCHES "mingw|generic")
    if (HAVE_LIBREADLINE AND HAVE_READLINE_H)
        list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_READLINE)
        list(APPEND LUA_LINK_LIBRARIES readline)
        message(STATUS "Readline support enabled")
    elseif (LUA_BUILD_LIBEDIT AND HAVE_LIBEDIT AND HAVE_EDITLINE_H)
        list(APPEND LUA_COMPILE_DEFINITIONS LUA_USE_READLINE)
        list(APPEND LUA_LINK_LIBRARIES edit)
        message(STATUS "Libedit support enabled")
    else ()
        message(WARNING "Readline requested but not found")
    endif ()
endif ()

#==============================================================================
# Source Files
#==============================================================================

set(LUA_LIB_SOURCES
        src/lapi.c src/lcode.c src/lctype.c src/ldebug.c src/ldo.c
        src/ldump.c src/lfunc.c src/lgc.c src/llex.c src/lmem.c
        src/lobject.c src/lopcodes.c src/lparser.c src/lstate.c src/lstring.c
        src/ltable.c src/ltm.c src/lundump.c src/lvm.c src/lzio.c
        src/lauxlib.c src/lbaselib.c src/lcorolib.c src/ldblib.c src/liolib.c
        src/lmathlib.c src/loadlib.c src/loslib.c src/lstrlib.c src/ltablib.c
        src/lutf8lib.c src/linit.c
)

set(LUA_PUBLIC_HEADERS
        src/lua.h src/luaconf.h src/lualib.h
        src/lauxlib.h src/lua.hpp
)

#==============================================================================
# Windows Resources
#==============================================================================

set(LUA_WIN_RESOURCES "")
if (WIN32)
    enable_language(RC OPTIONAL)

    if (CMAKE_RC_COMPILER)
        message(STATUS "Windows RC compiler found: ${CMAKE_RC_COMPILER}")

        # Extract version information from lua.h
        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/lua.h")
            file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/lua.h" LUA_H_CONTENT)

            foreach (var COPYRIGHT AUTHORS RELEASE)
                string(REGEX MATCH "#define LUA_${var}[[:space:]]+\"([^\"]+)\"" MATCH "${LUA_H_CONTENT}")
                if (MATCH)
                    string(REGEX REPLACE "#define LUA_${var}[[:space:]]+\"([^\"]+)\"" "\\1"
                            LUA_${var}_FROM_HEADER "${MATCH}")
                else ()
                    # Fallback values
                    if (var STREQUAL "COPYRIGHT")
                        set(LUA_${var}_FROM_HEADER "Copyright (C) 1994-2025 Lua.org, PUC-Rio")
                    elseif (var STREQUAL "AUTHORS")
                        set(LUA_${var}_FROM_HEADER "R. Ierusalimschy, L. H. de Figueiredo, W. Celes")
                    else ()
                        set(LUA_${var}_FROM_HEADER "Lua ${PROJECT_VERSION}")
                    endif ()
                endif ()
            endforeach ()

            message(STATUS "Extracted copyright: ${LUA_COPYRIGHT_FROM_HEADER}")
        endif ()

        # Configure version resource if template exists
        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lua_version.rc.in")
            configure_file(
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lua_version.rc.in"
                    "${CMAKE_CURRENT_BINARY_DIR}/lua_version.rc"
                    @ONLY
            )
            set(LUA_WIN_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/lua_version.rc")
        endif ()
    else ()
        message(STATUS "Windows RC compiler not available")
    endif ()
endif ()

#==============================================================================
# Target Creation Functions
#==============================================================================

# Function to setup common Lua target properties
function(lua_setup_target target_name)
    target_compile_definitions(${target_name}
            PRIVATE
            ${LUA_COMPILE_DEFINITIONS}
            $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
            $<$<AND:$<EQUAL:${CMAKE_SIZEOF_VOID_P},4>,$<PLATFORM_ID:Linux,Darwin,FreeBSD>>:_FILE_OFFSET_BITS=64>
    )

    target_compile_options(${target_name} PRIVATE ${LUA_COMPILE_OPTIONS})

    if (LUA_LINK_LIBRARIES)
        target_link_libraries(${target_name} PUBLIC ${LUA_LINK_LIBRARIES})
    endif ()

    target_include_directories(${target_name}
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
endfunction()

# Function to create a library target from object library
function(lua_create_library target_name lib_type output_name)
    add_library(${target_name} ${lib_type} $<TARGET_OBJECTS:liblua_objects>)

    # Apply the same setup as the object library (without creating dependency)
    lua_setup_target(${target_name})

    # Set library-specific properties
    set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${output_name})

    if (lib_type STREQUAL "STATIC")
        set_target_properties(${target_name} PROPERTIES
                ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                POSITION_INDEPENDENT_CODE ON
        )
    elseif (lib_type STREQUAL "SHARED")
        set_target_properties(${target_name} PROPERTIES
                SOVERSION ${PROJECT_VERSION_MAJOR}
                VERSION ${PROJECT_VERSION}
                LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        )

        # Windows DLL configuration
        if (WIN32)
            target_compile_definitions(${target_name}
                    PRIVATE LUA_BUILD_AS_DLL
                    INTERFACE LUA_BUILD_AS_DLL
            )
            set_target_properties(${target_name} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

            # Add Windows resources
            if (LUA_WIN_RESOURCES)
                target_sources(${target_name} PRIVATE ${LUA_WIN_RESOURCES})
            endif ()
        endif ()
    endif ()
endfunction()

# Function to create an executable target from object library
function(lua_create_executable target_name exe_objects output_name)
    add_executable(${target_name} $<TARGET_OBJECTS:${exe_objects}>)
    target_link_libraries(${target_name} PRIVATE ${LUA_TARGET})

    set_target_properties(${target_name} PROPERTIES
            OUTPUT_NAME ${output_name}
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

    # Add Windows resources
    if (WIN32 AND CMAKE_RC_COMPILER)
        # Determine base name from exe_objects
        string(REGEX REPLACE "_exe_objects$" "" base_name "${exe_objects}")

        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${base_name}_exe.rc.in")
            configure_file(
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${base_name}_exe.rc.in"
                    "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_exe.rc"
                    @ONLY
            )
            target_sources(${target_name} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_exe.rc")
        endif ()
    endif ()
endfunction()

#==============================================================================
# Object Libraries (Compile Once)
#==============================================================================

# Core Lua library object
add_library(liblua_objects OBJECT ${LUA_LIB_SOURCES})
lua_setup_target(liblua_objects)

# Executable object libraries
add_library(lua_exe_objects OBJECT src/lua.c)
add_library(luac_exe_objects OBJECT src/luac.c)

#==============================================================================
# Library Targets
#==============================================================================

set(LUA_ALL_TARGETS "")

# Static libraries
if (LUA_BUILD_STATIC)
    set(static_base_name $<IF:$<BOOL:${LUA_BUILD_SHARED}>,lua-static,lua>)

    lua_create_library(liblua_static STATIC "lua$<IF:$<BOOL:${LUA_BUILD_SHARED}>,-static,>")
    add_library(lua::lua_static ALIAS liblua_static)
    list(APPEND LUA_ALL_TARGETS liblua_static)

    # Versioned static libraries
    if (LUA_VERSIONED_INSTALL)
        lua_create_library(liblua_static_dot STATIC
                "lua${LUA_VERSION_SUFFIX_DOT}$<IF:$<BOOL:${LUA_BUILD_SHARED}>,-static,>")
        lua_create_library(liblua_static_compact STATIC
                "lua${LUA_VERSION_SUFFIX_COMPACT}$<IF:$<BOOL:${LUA_BUILD_SHARED}>,-static,>")
        list(APPEND LUA_ALL_TARGETS liblua_static_dot liblua_static_compact)
    endif ()
endif ()

# Shared libraries
if (LUA_BUILD_SHARED)
    lua_create_library(liblua_shared SHARED "lua")
    add_library(lua::lua_shared ALIAS liblua_shared)
    list(APPEND LUA_ALL_TARGETS liblua_shared)

    # Versioned shared libraries
    if (LUA_VERSIONED_INSTALL)
        lua_create_library(liblua_shared_dot SHARED "lua${LUA_VERSION_SUFFIX_DOT}")
        lua_create_library(liblua_shared_compact SHARED "lua${LUA_VERSION_SUFFIX_COMPACT}")
        list(APPEND LUA_ALL_TARGETS liblua_shared_dot liblua_shared_compact)
    endif ()
endif ()

# Set default library target
if (LUA_BUILD_SHARED)
    set(LUA_TARGET liblua_shared)
    add_library(lua::lua ALIAS liblua_shared)
else ()
    set(LUA_TARGET liblua_static)
    add_library(lua::lua ALIAS liblua_static)
endif ()

#==============================================================================
# Executable Targets
#==============================================================================

# Main executables
lua_create_executable(lua lua_exe_objects "lua")
lua_create_executable(luac luac_exe_objects "luac")
list(APPEND LUA_ALL_TARGETS lua luac)

# Versioned executables
if (LUA_VERSIONED_INSTALL)
    lua_create_executable(lua_dot lua_exe_objects "lua${LUA_VERSION_SUFFIX_DOT}")
    lua_create_executable(lua_compact lua_exe_objects "lua${LUA_VERSION_SUFFIX_COMPACT}")
    lua_create_executable(luac_dot luac_exe_objects "luac${LUA_VERSION_SUFFIX_DOT}")
    lua_create_executable(luac_compact luac_exe_objects "luac${LUA_VERSION_SUFFIX_COMPACT}")
    list(APPEND LUA_ALL_TARGETS lua_dot lua_compact luac_dot luac_compact)
endif ()

#==============================================================================
# Testing
#==============================================================================

if (LUA_BUILD_TESTS)
    enable_testing()

    # Helper function to add tests for an executable
    function(lua_add_tests exe_name)
        add_test(NAME ${exe_name}_version COMMAND ${exe_name} -v WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
        add_test(NAME ${exe_name}c_help COMMAND ${exe_name}c -h WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

        file(WRITE ${CMAKE_BINARY_DIR}/test_basic_${exe_name}.lua "print('Hello from ${exe_name}!')")
        add_test(NAME ${exe_name}_basic_script
                COMMAND ${exe_name} test_basic_${exe_name}.lua
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set_tests_properties(${exe_name}_basic_script PROPERTIES
                PASS_REGULAR_EXPRESSION "Hello from ${exe_name}!")
    endfunction()

    # Add tests for all executables
    lua_add_tests(lua)
    if (LUA_VERSIONED_INSTALL)
        lua_add_tests(lua${LUA_VERSION_SUFFIX_DOT})
        lua_add_tests(lua${LUA_VERSION_SUFFIX_COMPACT})
    endif ()
endif ()

#==============================================================================
# Installation
#==============================================================================

# Install all targets
install(TARGETS ${LUA_ALL_TARGETS}
        EXPORT luaTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
)

# Install headers
install(FILES ${LUA_PUBLIC_HEADERS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT Development
)

# Install documentation
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/doc/lua.1")
    install(FILES doc/lua.1 doc/luac.1
            DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
            COMPONENT Documentation
    )
endif ()

# Create Lua module directories
install(DIRECTORY DESTINATION
        ${CMAKE_INSTALL_DATAROOTDIR}/lua/${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        COMPONENT Runtime
)
install(DIRECTORY DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/lua/${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        COMPONENT Runtime
)

#==============================================================================
# Export Configuration
#==============================================================================

# Export targets
install(EXPORT luaTargets
        FILE luaTargets.cmake
        NAMESPACE lua::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lua
        COMPONENT Development
)

# Create config file
configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/luaConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/luaConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lua
        PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR
)

# Create version file
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/luaConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

# Install config files
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/luaConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/luaConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lua
        COMPONENT Development
)

# Create pkg-config file
configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lua.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lua.pc"
        @ONLY
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/lua.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
        COMPONENT Development
)

#==============================================================================
# Custom Targets
#==============================================================================

# Local installation target
add_custom_target(local
        COMMAND ${CMAKE_COMMAND}
        -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/install
        -P ${CMAKE_BINARY_DIR}/cmake_install.cmake
        COMMENT "Installing to local directory"
)

# Configuration echo target
add_custom_target(echo
        COMMAND ${CMAKE_COMMAND} -E echo "=== Lua Build Configuration ==="
        COMMAND ${CMAKE_COMMAND} -E echo "Platform: ${LUA_PLATFORM}"
        COMMAND ${CMAKE_COMMAND} -E echo "System: ${CMAKE_SYSTEM_NAME}"
        COMMAND ${CMAKE_COMMAND} -E echo "Compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}"
        COMMAND ${CMAKE_COMMAND} -E echo "C Standard: C${CMAKE_C_STANDARD}"
        COMMAND ${CMAKE_COMMAND} -E echo "Version: ${PROJECT_VERSION}"
        COMMAND ${CMAKE_COMMAND} -E echo "Versioned install: ${LUA_VERSIONED_INSTALL}"
        COMMAND ${CMAKE_COMMAND} -E echo "$<$<BOOL:${LUA_VERSIONED_INSTALL}>:Version suffixes: ${LUA_VERSION_SUFFIX_DOT}, ${LUA_VERSION_SUFFIX_COMPACT}>"
        COMMAND ${CMAKE_COMMAND} -E echo "Shared library: ${LUA_BUILD_SHARED}"
        COMMAND ${CMAKE_COMMAND} -E echo "Static library: ${LUA_BUILD_STATIC}"
        COMMAND ${CMAKE_COMMAND} -E echo "Readline: ${LUA_BUILD_READLINE}"
        COMMAND ${CMAKE_COMMAND} -E echo "LTO: ${LUA_ENABLE_LTO}"
        COMMAND ${CMAKE_COMMAND} -E echo "Install prefix: ${CMAKE_INSTALL_PREFIX}"
        COMMENT "Displaying build configuration"
)

#==============================================================================
# Packaging
#==============================================================================

set(CPACK_PACKAGE_NAME "lua")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_VENDOR "Lua.org")
set(CPACK_PACKAGE_CONTACT "info@lua.org")

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/doc/readme.html")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/doc/readme.html")
endif ()

# Platform-specific packaging
if (WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
elseif (APPLE)
    set(CPACK_GENERATOR "TGZ;DragNDrop")
else ()
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
endif ()

# Component descriptions
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "Lua Runtime")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "Lua interpreter and compiler executables")
set(CPACK_COMPONENT_DEVELOPMENT_DISPLAY_NAME "Lua Development")
set(CPACK_COMPONENT_DEVELOPMENT_DESCRIPTION "Headers and libraries for Lua development")
set(CPACK_COMPONENT_DOCUMENTATION_DISPLAY_NAME "Lua Documentation")
set(CPACK_COMPONENT_DOCUMENTATION_DESCRIPTION "Manual pages and documentation")

include(CPack)

#==============================================================================
# Configuration Summary
#==============================================================================

message(STATUS "")
message(STATUS "=== Lua Configuration Summary ===")
message(STATUS "Platform: ${LUA_PLATFORM}")
message(STATUS "C Standard: C${CMAKE_C_STANDARD}")
message(STATUS "Shared library: ${LUA_BUILD_SHARED}")
message(STATUS "Static library: ${LUA_BUILD_STATIC}")
message(STATUS "Versioned install: ${LUA_VERSIONED_INSTALL}")
if (LUA_VERSIONED_INSTALL)
    message(STATUS "Version suffixes: ${LUA_VERSION_SUFFIX_DOT}, ${LUA_VERSION_SUFFIX_COMPACT}")
endif ()
message(STATUS "Readline support: ${LUA_BUILD_READLINE}")
message(STATUS "Tests: ${LUA_BUILD_TESTS}")
message(STATUS "Link Time Optimization: ${LUA_ENABLE_LTO}")
if (WIN32 AND CMAKE_RC_COMPILER AND LUA_COPYRIGHT_FROM_HEADER)
    message(STATUS "Windows version info: Enabled")
endif ()
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "================================")
message(STATUS "")
