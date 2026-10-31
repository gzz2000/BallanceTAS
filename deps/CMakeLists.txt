# Dependencies are usually required only when building the full mod (BUILD_MOD).
# Lua is also required by the standalone tas_lua_runtime tests.
if(BUILD_MOD OR BUILD_TESTS)

# lua
add_subdirectory(lua EXCLUDE_FROM_ALL)

endif()

if(BUILD_MOD)

# MinHook is an x86/x64 Windows instruction patcher.  The native macOS port
# uses an instance-vtable hook and deliberately does not compile MinHook.
if(WIN32)
    add_subdirectory(minhook EXCLUDE_FROM_ALL)
endif()

# zip
if(NOT TARGET zip)
    add_subdirectory(zip EXCLUDE_FROM_ALL)
endif()

# fmt
add_subdirectory(fmt EXCLUDE_FROM_ALL)
if(EMSCRIPTEN)
    # fmt 11.2 relies on transitive malloc/free declarations removed by the
    # newer libc++ shipped with emsdk 6. Keep this dependency fix localized.
    target_compile_options(fmt PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdlib>")
endif()

# yyjson
if(NOT TARGET yyjson)
    add_subdirectory(yyjson EXCLUDE_FROM_ALL)
endif()

endif() # BUILD_MOD
