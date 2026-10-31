# =============================================================================
# BallanceTAS – Modular Build
#
# The sources are split into focused STATIC library targets. Each module has
# explicit dependency declarations so that:
#   1. Changing a file only rebuilds the modules that actually depend on it.
#   2. The allowed #include directions are enforced via target_link_libraries.
#   3. New subsystems can be added to one module without touching the others.
#
# All static libraries are linked into the final BallanceTAS shared library
# (.bmodp DLL loaded by BML).
# =============================================================================

# Helper: every module can #include files in src/ and its module folders.
set(TAS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(TAS_MODULE_INCLUDE_DIRS
        ${TAS_SRC_DIR}
        ${TAS_SRC_DIR}/Core
        ${TAS_SRC_DIR}/Game
        ${TAS_SRC_DIR}/Runtime
        ${TAS_SRC_DIR}/Scripting
        ${TAS_SRC_DIR}/Workflow
        ${TAS_SRC_DIR}/UI
        ${TAS_SRC_DIR}/Host
)

# ── Module: core ─────────────────────────────────────────────────────────────
#    Pure C++ infrastructure – no Virtools and no Lua dependency.
# ─────────────────────────────────────────────────────────────────────────────
add_library(tas_core STATIC
        Core/Result.h
        Core/PlaybackTypes.h
        Core/EventBus.h
        Core/GameEvents.h
        Runtime/RuntimeSession.h    Runtime/RuntimeSession.cpp
        Core/TASConstants.h
        Core/ILogSink.h
        Core/ServiceContainer.h     Core/ServiceContainer.cpp
        Core/Logger.h               Core/Logger.cpp
        Core/ThreadOwnershipValidator.h
        Core/LockFreeMPSCQueue.h
        Core/ResourceManager.h      Core/ResourceManager.cpp
)
target_include_directories(tas_core
        PUBLIC  ${TAS_MODULE_INCLUDE_DIRS}
)
# tas_core is now pure C++ — no BML / VirtoolsSDK dependency.
set_target_properties(tas_core PROPERTIES FOLDER "TAS/Modules")

# ── Module: lua_runtime ──────────────────────────────────────────────────────
#    Project-owned Lua C API abstraction.
add_library(tas_lua_runtime STATIC
        LuaRuntime/LuaState.h          LuaRuntime/LuaState.cpp
        LuaRuntime/LuaStackGuard.h
        LuaRuntime/LuaRef.h            LuaRuntime/LuaRef.cpp
        LuaRuntime/LuaFunction.h       LuaRuntime/LuaFunction.cpp
        LuaRuntime/LuaThread.h         LuaRuntime/LuaThread.cpp
        LuaRuntime/LuaProtectedCall.h  LuaRuntime/LuaProtectedCall.cpp
        LuaRuntime/LuaValue.h          LuaRuntime/LuaValue.cpp
        LuaRuntime/LuaYieldState.h
        LuaRuntime/LuaUserdata.h
        LuaRuntime/LuaBinder.h
)
target_link_libraries(tas_lua_runtime
        PUBLIC tas_core lua::lua
)
target_include_directories(tas_lua_runtime
        PUBLIC ${TAS_MODULE_INCLUDE_DIRS}
)
set_target_properties(tas_lua_runtime PROPERTIES FOLDER "TAS/Modules")

# ── Module: game ─────────────────────────────────────────────────────────────
#    Everything below requires VirtoolsSDK & BML → only built with BUILD_MOD.
# ─────────────────────────────────────────────────────────────────────────────
if(BUILD_MOD)
add_library(tas_game STATIC
        Game/GameInterface.h        Game/GameInterface.cpp
        Game/IGameControl.h
        Game/IGameQuery.h
        Game/IInputAccess.h
        Game/IPhysicsProvider.h
        Game/IObjectProvider.h
        Game/IGameStateProvider.h
        Game/ITimeProvider.h
        Game/physics_RT.h
        $<$<PLATFORM_ID:Windows>:Game/physics_RT.cpp>
        Game/PhysicsDeterminism.h   Game/PhysicsDeterminism.cpp
        Game/InputSystem.h          Game/InputSystem.cpp
        Game/TASHook.h
        Game/Hook.h
)
target_link_libraries(tas_game
        PUBLIC  tas_core BML CK2 VxMath
        $<$<PLATFORM_ID:Windows>:minhook>
        $<$<PLATFORM_ID:Darwin>:physics_RT>
)
set_target_properties(tas_game PROPERTIES FOLDER "TAS/Modules")

# ── Module: scripting ────────────────────────────────────────────────────────
#    Lua execution runtime – contexts, scheduler, inter-context comms.
# ─────────────────────────────────────────────────────────────────────────────
add_library(tas_scripting STATIC
        Scripting/ScriptContextTypes.h
        Scripting/ScriptContext.h   Scripting/ScriptContext.cpp
        Scripting/ScriptContextManager.h Scripting/ScriptContextManager.cpp
        Scripting/LuaScheduler.h    Scripting/LuaScheduler.cpp
        Scripting/EventManager.h    Scripting/EventManager.cpp
        Scripting/SharedDataManager.h Scripting/SharedDataManager.cpp
        Scripting/MessageBus.h      Scripting/MessageBus.cpp
        Scripting/SharedBuffer.h
        Scripting/AsyncTask.h       Scripting/AsyncTask.cpp
)
target_link_libraries(tas_scripting
        PUBLIC  tas_core tas_lua_runtime lua::lua yyjson
        PRIVATE tas_game
)
set_target_properties(tas_scripting PROPERTIES FOLDER "TAS/Modules")

# ── Module: features ─────────────────────────────────────────────────────────
#    TAS operation layer – engine coordinator, services, recording, playback,
#    savestate, project management.
# ─────────────────────────────────────────────────────────────────────────────
add_library(tas_features STATIC
        Workflow/TASEngine.h        Workflow/TASEngine.cpp
        Workflow/EngineBootstrap.h  Workflow/EngineBootstrap.cpp
        Runtime/OperationRequestStore.h
        Runtime/RuntimeEventRouter.h Runtime/RuntimeEventRouter.cpp
        Runtime/ContextLifecycleCoordinator.h Runtime/ContextLifecycleCoordinator.cpp
        Runtime/LuaTypedEventBridge.h Runtime/LuaTypedEventBridge.cpp
        Workflow/HookManager.h      Workflow/HookManager.cpp
        Workflow/ConfigService.h    Workflow/ConfigService.cpp
        Workflow/TASProject.h       Workflow/TASProject.cpp
        Workflow/ProjectManager.h   Workflow/ProjectManager.cpp
        Workflow/Recorder.h         Workflow/Recorder.cpp
        Workflow/RecordPlayer.h     Workflow/RecordPlayer.cpp
        Workflow/RecordAnnotations.cpp
        Workflow/RecordFileIO.h     Workflow/RecordFileIO.cpp
        Workflow/RecordingService.h Workflow/RecordingService.cpp
        Workflow/PlaybackService.h  Workflow/PlaybackService.cpp
        Workflow/TranslationService.h Workflow/TranslationService.cpp
        Workflow/ScriptInputTransition.h Workflow/ScriptInputTransition.cpp
        Workflow/ValidationService.h Workflow/ValidationService.cpp
        Workflow/ValidationComparison.cpp
        Workflow/ScriptGenerator.h  Workflow/ScriptGenerator.cpp
        Workflow/ScriptGenerationCore.h Workflow/ScriptGenerationCore.cpp
        Workflow/StartupProjectManager.h Workflow/StartupProjectManager.cpp
        Workflow/HarnessProtocol.h Workflow/HarnessProtocol.cpp
        Workflow/HarnessRuntime.h Workflow/HarnessRuntime.cpp
        Host/TasHostApi.h
        Host/PlayerHostBridge.h Host/PlayerHostBridge.cpp
        Workflow/SavestateManager.h Workflow/SavestateManager.cpp
        Workflow/LuaREPLServer.h    Workflow/LuaREPLServer.cpp
)
target_link_libraries(tas_features
        PUBLIC  tas_core tas_game tas_scripting
        PRIVATE yyjson zip fmt::fmt
)
set_target_properties(tas_features PROPERTIES FOLDER "TAS/Modules")

# ── Module: lua_bindings ─────────────────────────────────────────────────────
#    All LuaApi/* binding translation units – isolated so that core/game
#    changes do not trigger a rebuild of ~30 binding files and vice versa.
# ─────────────────────────────────────────────────────────────────────────────
add_library(tas_lua_bindings STATIC
        LuaApi/LuaApi.h
        LuaApi/LuaApi.cpp
        LuaApi/LuaApi_Core.cpp
        LuaApi/LuaApi_CKEnums.cpp
        LuaApi/LuaApi_CKObject.cpp
        LuaApi/LuaApi_CKSceneObject.cpp
        LuaApi/LuaApi_CKBeObject.cpp
        LuaApi/LuaApi_CKRenderObject.cpp
        LuaApi/LuaApi_CK3dEntity.cpp
        LuaApi/LuaApi_CKCamera.cpp
        LuaApi/LuaApi_PhysicsObject.cpp
        LuaApi/LuaApi_Savestate.cpp
        LuaApi/LuaApi_Validation.cpp
        LuaApi/LuaApi_GC.cpp
        LuaApi/LuaApi_Debug.cpp
        LuaApi/LuaApi_Result.cpp
        LuaApi/LuaApi_Project.cpp
        LuaApi/LuaApi_Record.cpp
        LuaApi/LuaApi_Level.cpp
        LuaApi/LuaApi_Menu.cpp
        LuaApi/LuaApi_Harness.cpp
        LuaApi/LuaApi_SharedBuffer.cpp
        LuaApi/LuaApi_Input.cpp
        LuaApi/LuaApi_VxVector.cpp
        LuaApi/LuaApi_VxColor.cpp
        LuaApi/LuaApi_VxRect.cpp
        LuaApi/LuaApi_VxQuaternion.cpp
        LuaApi/LuaApi_VxMatrix.cpp
        LuaApi/LuaApi_VxIntersectionDesc.cpp
        LuaApi/LuaApi_World.cpp
        LuaApi/LuaApi_Concurrency.cpp
        LuaApi/LuaApi_Async.cpp
        LuaApi/LuaApi_Context.cpp
        LuaApi/LuaApi_Prelude.cpp
        LuaApi/LuaApiCommon.h       LuaApi/LuaApiCommon.cpp
        LuaApi/LuaApiValues.h       LuaApi/LuaApiValues.cpp
        LuaApi/LuaApiScheduler.cpp
)
target_link_libraries(tas_lua_bindings
        PRIVATE tas_core tas_game tas_scripting tas_features fmt::fmt
)
set_target_properties(tas_lua_bindings PROPERTIES FOLDER "TAS/Modules")

# ── Module: ui ───────────────────────────────────────────────────────────────
#    User interface layer (TAS menu, in-game OSD).
# ─────────────────────────────────────────────────────────────────────────────
add_library(tas_ui STATIC
        UI/UIManager.h              UI/UIManager.cpp
        $<$<BOOL:${TAS_ENABLE_LEGACY_MENU}>:UI/TASMenu.h>
        $<$<BOOL:${TAS_ENABLE_LEGACY_MENU}>:UI/TASMenu.cpp>
        UI/TASMenuPresentation.h    UI/TASMenuPresentation.cpp
        UI/InGameOSD.h              UI/InGameOSD.cpp
)
target_compile_definitions(tas_ui PUBLIC
        TAS_ENABLE_LEGACY_MENU=$<BOOL:${TAS_ENABLE_LEGACY_MENU}>)
target_link_libraries(tas_ui
        PUBLIC  tas_core
        PRIVATE tas_features BML
)
set_target_properties(tas_ui PROPERTIES FOLDER "TAS/Modules")

# ── Final shared library (.bmodp) ────────────────────────────────────────────
#    BML mod entry point – assembles all modules into the loadable plugin DLL.
# ─────────────────────────────────────────────────────────────────────────────
add_library(BallanceTAS SHARED
        BMLLogSink.h
        BallanceTAS.h               BallanceTAS.cpp
)

target_include_directories(BallanceTAS
        PUBLIC
        $<BUILD_INTERFACE:${TAS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(BallanceTAS
        PRIVATE
        tas_core
        tas_game
        tas_scripting
        tas_features
        tas_lua_bindings
        tas_ui
)

target_compile_options(BallanceTAS PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:char8_t->
)

set_target_properties(BallanceTAS PROPERTIES SUFFIX ".bmodp")
if(APPLE)
    # Diagnostic-only native rasterizer API; absent from normal mod builds.
    set(TAS_PIXEL_TRACE_INCLUDE_DIR "" CACHE PATH "Native CKRasterizer headers for optional pixel diagnostics")
    if(TAS_PIXEL_TRACE_INCLUDE_DIR)
        if(NOT EXISTS "${TAS_PIXEL_TRACE_INCLUDE_DIR}/CKRasterizer.h")
            message(FATAL_ERROR "TAS_PIXEL_TRACE_INCLUDE_DIR must contain CKRasterizer.h")
        endif()
        target_include_directories(BallanceTAS PRIVATE "${TAS_PIXEL_TRACE_INCLUDE_DIR}")
        target_compile_definitions(BallanceTAS PRIVATE TAS_ENABLE_PIXEL_TRACE=1)
    endif()
    # Mods are installed under Application Support, while their dependencies
    # live beside the native Player inside the application bundle.
    set_target_properties(BallanceTAS PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@executable_path/../Plugins;@executable_path/../BuildingBlocks;@executable_path/../Bin")
endif()

endif() # BUILD_MOD
