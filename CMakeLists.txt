cmake_minimum_required(VERSION 3.14)

project(BallanceTAS
        VERSION 2.0.0
        LANGUAGES CXX
        DESCRIPTION "The TAS system for Ballance "
        HOMEPAGE_URL https://github.com/doyaGu/BallanceTAS)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS YES)

# Add path for custom modules
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GenerateExportHeader)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Use folders to organize targets in an IDE
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMakeTargets")

# Use relative paths
if (WIN32)
    set(CMAKE_SUPPRESS_REGENERATION TRUE)
endif ()

# Define build type if not specified
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as no build type was specified")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type (Debug/Release)" FORCE)
    # Define standard build type options for GUI
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif ()

# Set default install path
if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${PROJECT_SOURCE_DIR}/install" CACHE PATH "The root directory of the installation" FORCE)
    message(STATUS "Setting default install directory to ${CMAKE_INSTALL_PREFIX} as no install directory was specified")
endif ()

# Disable msvc unsafe warnings globally
add_compile_definitions(
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_NONSTDC_NO_WARNINGS>
        $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>
        $<$<AND:$<PLATFORM_ID:Windows>,$<CXX_COMPILER_ID:Clang>>:NOMINMAX>
)

# Sources are UTF-8; without /utf-8 MSVC reads them in the system code page (C4819).
add_compile_options(
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:char8_t->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

set(TAS_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include" CACHE INTERNAL "")
set(TAS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/src" CACHE INTERNAL "")

# ---------------------------------------------------------------------------
#  Build options
# ---------------------------------------------------------------------------
option(BUILD_MOD    "Build the full BallanceTAS mod (requires VirtoolsSDK & BML)" ON)
option(BUILD_TESTS  "Build unit tests" ON)
option(TAS_ENABLE_LEGACY_MENU
       "Build the TAS project menu using the BML composition API"
       ON)
set(TAS_BALLANCED_ROOT "" CACHE PATH "Ballanced checkout used by the native macOS build")
set(TAS_BML_ROOT "" CACHE PATH "BallanceModLoaderPlus checkout used by the native macOS build")

# Enable CTest at the top level so `ctest` discovers tests from the build root.
if(BUILD_TESTS)
    include(CTest)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
#  SDK dependencies — only required when building the full mod
# ---------------------------------------------------------------------------
if(BUILD_MOD)
    if(APPLE AND TAS_BALLANCED_ROOT AND TAS_BML_ROOT)
        # Native SDK headers and libraries must describe the same runtime.
        # Permit a staged/candidate SDK without replacing the checkouts' shared
        # build outputs (or silently linking an older library against new headers).
        set(TAS_CK2_LIBRARY "${TAS_BALLANCED_ROOT}/build/macos-arm64-runtime/lib/libCK2.dylib"
            CACHE FILEPATH "Native CK2 library matching the selected Ballanced headers")
        set(TAS_VXMATH_LIBRARY "${TAS_BALLANCED_ROOT}/build/macos-arm64-runtime/lib/libVxMath.dylib"
            CACHE FILEPATH "Native VxMath library matching the selected Ballanced headers")
        set(TAS_PHYSICS_LIBRARY "${TAS_BALLANCED_ROOT}/build/macos-arm64-runtime/lib/libphysics_RT.dylib"
            CACHE FILEPATH "Native physics_RT library matching the selected Ballanced headers")
        set(TAS_BML_LIBRARY "${TAS_BML_ROOT}/build-macos-arm64-native/lib/libBMLPlus.dylib"
            CACHE FILEPATH "Native BML library matching the selected BML SDK headers")
        set(TAS_BML_INCLUDE_DIR "${TAS_BML_ROOT}/install/include"
            CACHE PATH "Native BML headers matching TAS_BML_LIBRARY")
        set(TAS_IVP_INCLUDE_DIR "${TAS_BALLANCED_ROOT}/build/macos-arm64-runtime/Source/BuildingBlocks/physics_RT/ivp/include/ivp"
            CACHE PATH "Generated IVP headers matching TAS_PHYSICS_LIBRARY")
        foreach(_library IN ITEMS CK2 VxMath)
            string(TOUPPER "${_library}" _library_key)
            add_library(${_library} SHARED IMPORTED)
            set_target_properties(${_library} PROPERTIES
                IMPORTED_LOCATION "${TAS_${_library_key}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${TAS_BALLANCED_ROOT}/Source/CK2/include;${TAS_BALLANCED_ROOT}/Source/VxMath/include")
        endforeach()
        add_library(BML SHARED IMPORTED)
        set_target_properties(BML PROPERTIES
            IMPORTED_LOCATION "${TAS_BML_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${TAS_BML_INCLUDE_DIR}")
        add_library(physics_RT SHARED IMPORTED)
        set_target_properties(physics_RT PROPERTIES
            IMPORTED_LOCATION "${TAS_PHYSICS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${TAS_BALLANCED_ROOT}/Source/BuildingBlocks/physics_RT;${TAS_IVP_INCLUDE_DIR}")
    else()
        find_package(VirtoolsSDK REQUIRED)
        find_package(BML REQUIRED)
        # Current SDK/BML packages export namespaced targets while the TAS
        # module graph historically links the unqualified target names.
        # Keep that graph portable without duplicating platform branches.
        if(TARGET VirtoolsSDK::CK2 AND NOT TARGET CK2)
            add_library(CK2 ALIAS VirtoolsSDK::CK2)
        endif()
        if(TARGET VirtoolsSDK::VxMath AND NOT TARGET VxMath)
            add_library(VxMath ALIAS VirtoolsSDK::VxMath)
        endif()
        if(TARGET BML::BML AND NOT TARGET BML)
            add_library(BML ALIAS BML::BML)
        endif()
    endif()
endif()

add_subdirectory(deps)
add_subdirectory(src)

# Enable tests
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
