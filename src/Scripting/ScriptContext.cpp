#include "ScriptContext.h"

#include <algorithm>
#include <filesystem>

#include "Logger.h"
#include "TASConstants.h"
#include "TASEngine.h"
#include "TASProject.h"
#include "LuaScheduler.h"
#include "LuaApi/LuaApi.h"
#include "LuaRuntime/LuaProtectedCall.h"
#include "LuaRuntime/LuaStackGuard.h"
#include "LuaRuntime/LuaValue.h"
#include "EventManager.h"
#include "HudWatches.h"
#include "InputSystem.h"
#include "ProjectManager.h"
#include "RecordPlayer.h"
#include "SavestateManager.h"
#include "ValidationService.h"
#include "ScriptContextManager.h"
#include "ServiceContainer.h"
#include "MessageBus.h"
#include "SharedDataManager.h"

static void AddLuaPath(lua_State *state, const std::string &path) {
    if (!state || path.empty()) {
        return;
    }

    lua_getglobal(state, "package");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return;
    }

    lua_getfield(state, -1, "path");
    const char *currentPath = lua_tostring(state, -1);
    std::string newPath = currentPath ? currentPath : "";
    lua_pop(state, 1);

    if (!newPath.empty()) {
        newPath += ";";
    }
    newPath += path + "/?.lua;";
    newPath += path + "/?/init.lua";

    lua_pushlstring(state, newPath.data(), newPath.size());
    lua_setfield(state, -2, "path");
    lua_pop(state, 1);
}

static tas::lua::LuaValue MakeGameEventValue(const LuaGameEvent &event) {
    using tas::lua::LuaValue;

    auto table = std::make_shared<LuaValue::Table>();
    auto addString = [&](std::string key, std::string value) {
        table->entries.push_back({LuaValue::Key{std::move(key)},
                                  std::make_shared<LuaValue>(LuaValue::Storage{std::move(value)})});
    };
    auto addInteger = [&](std::string key, lua_Integer value) {
        table->entries.push_back({LuaValue::Key{std::move(key)},
                                  std::make_shared<LuaValue>(LuaValue::Storage{value})});
    };

    addInteger("type", static_cast<lua_Integer>(event.type));
    addString("name", event.name);
    addInteger("tick", static_cast<lua_Integer>(event.tick));

    if (event.sector.has_value()) {
        addInteger("sector", static_cast<lua_Integer>(*event.sector));
    }
    if (event.points.has_value()) {
        addInteger("points", static_cast<lua_Integer>(*event.points));
    }
    if (event.lifeCount.has_value()) {
        addInteger("life_count", static_cast<lua_Integer>(*event.lifeCount));
    }

    return LuaValue(LuaValue::Storage{table});
}

ScriptContext::ScriptContext(TASEngine *engine, std::string name, ScriptContextType type, int priority)
    : m_Engine(engine), m_Name(std::move(name)), m_Type(type), m_Priority(priority) {
    if (!m_Engine) {
        throw std::runtime_error("ScriptContext requires a valid TASEngine instance.");
    }
    if (m_Name.empty()) {
        throw std::runtime_error("ScriptContext requires a non-empty name.");
    }
}

ScriptContext::~ScriptContext() {
    Shutdown();
}

bool ScriptContext::Initialize() {
    m_ThreadValidator.AssertOwnership();

    if (m_IsInitialized) {
        Log::Warn("[%s] ScriptContext already initialized.", m_Name.c_str());
        return true;
    }

    Log::Info("[%s] Initializing ScriptContext...", m_Name.c_str());

    try {
        // 1. Initialize Lua State (independent VM for this context)
        m_LuaState.OpenStandardLibraries();

        // 2. Create Lua Scheduler (independent scheduler for this context)
        // Pass 'this' context for proper context isolation
        m_Scheduler = std::make_unique<LuaScheduler>(m_Engine, this);

        // 3. Create Event Manager (independent event manager for this context)
        m_EventManager = std::make_unique<EventManager>();

        // 4. Create Input System (independent input system for this context)
        m_InputSystem = std::make_unique<InputSystem>();
        m_InputSystem->SetEnabled(true);

        // 5. Register Lua APIs and configure script resolution for this context.
        LuaApi::Register(this);
        lua_State *state = m_LuaState.Get();
        AddLuaPath(state, m_Engine->GetPath());
        std::string luaSubPath = (std::filesystem::path(m_Engine->GetPath()) / "lua").string();
        AddLuaPath(state, luaSubPath);

        // 6. Set default GC mode (Generational for TAS workloads)
        SetGCMode(LuaGCMode::Generational);

        m_IsInitialized = true;
        Log::Info("[%s] ScriptContext initialized successfully.", m_Name.c_str());
        return true;
    } catch (const std::exception &e) {
        Log::Error("[%s] Failed to initialize ScriptContext: %s", m_Name.c_str(), e.what());
        return false;
    }
}

void ScriptContext::Shutdown() {
    m_ThreadValidator.AssertOwnership();

    if (!m_IsInitialized) return;

    if (m_Engine == nullptr) {
        Log::Error("[%s] Cannot shutdown ScriptContext: TASEngine instance is null.", m_Name.c_str());
        return;
    }

    Log::Info("[%s] Shutting down ScriptContext...", m_Name.c_str());

    try {
        // Clean up inter-context communication registrations
        auto *contextManager = GetScriptContextManager();
        if (contextManager) {
            // Remove all message handlers for this context
            auto *messageBus = contextManager->GetMessageBus();
            if (messageBus) {
                messageBus->RemoveAllHandlers(m_Name);
            }

            // Remove all shared data watches for this context
            auto *sharedData = contextManager->GetSharedData();
            if (sharedData) {
                sharedData->UnwatchAll(m_Name);
            }
        }
        if (auto *hud = m_Engine->GetServiceProvider().Resolve<HudWatches>()) {
            hud->ClearOwner(m_Name);
        }

        // Stop any running script
        Stop();

        // Shutdown input system
        if (m_InputSystem) {
            m_InputSystem->Reset();
            m_InputSystem.reset();
        }

        // Shutdown event manager
        if (m_EventManager) {
            m_EventManager->ClearListeners();
            m_EventManager.reset();
        }

        ClearGameEventListeners();

        // Shutdown scheduler
        if (m_Scheduler) {
            m_Scheduler->Clear();
            m_Scheduler.reset();
        }

        // Mark as uninitialized before destroying Lua state
        // This prevents any code from trying to use the context during Lua state destruction
        m_IsInitialized = false;

        // Recreate the Lua state after all registry refs owned by this context are gone.
        m_LuaState = tas::lua::LuaState{};

        Log::Info("[%s] ScriptContext shutdown complete.", m_Name.c_str());
    } catch (const std::exception &e) {
        if (m_Engine) {
            Log::Error("[%s] Exception during ScriptContext shutdown: %s", m_Name.c_str(), e.what());
        }
    }
}

bool ScriptContext::Reinitialize(const std::string &newName, int newPriority) {
    m_ThreadValidator.AssertOwnership();

    if (!m_IsInitialized) {
        Log::Error("[%s] Cannot reinitialize an uninitialized ScriptContext.", m_Name.c_str());
        return false;
    }

    Log::Info("[%s] Reinitializing ScriptContext as '%s' with priority %d...",
              m_Name.c_str(), newName.c_str(), newPriority);

    try {
        Shutdown();
        m_Name = newName;
        m_Priority = newPriority;
        if (!Initialize()) {
            return false;
        }

        Log::Info("[%s] ScriptContext reinitialized successfully.", m_Name.c_str());
        return true;
    } catch (const std::exception &e) {
        Log::Error("[%s] Failed to reinitialize ScriptContext: %s",
                   m_Name.c_str(), e.what());
        return false;
    }
}

bool ScriptContext::LoadAndExecute(TASProject *project) {
    m_ThreadValidator.AssertOwnership();

    if (!m_IsInitialized) {
        Log::Error("[%s] ScriptContext not initialized.", m_Name.c_str());
        return false;
    }

    if (!project || !project->IsScriptProject() || !project->IsValid()) {
        Log::Error("[%s] Invalid script project provided to ScriptContext.", m_Name.c_str());
        return false;
    }

    // Stop any currently running script and rebuild the Lua VM so reload cannot
    // inherit globals, package.loaded entries, registry refs, or callbacks.
    Stop();
    Shutdown();
    if (!Initialize()) {
        Log::Error("[%s] Failed to reset Lua VM before loading project '%s'.",
                   m_Name.c_str(), project->GetName().c_str());
        return false;
    }

    try {
        // Prepare project for execution
        std::string executionPath = PrepareProjectForExecution(project);
        if (executionPath.empty()) {
            Log::Error("[%s] Failed to prepare script project for execution: %s",
                       m_Name.c_str(), project->GetName().c_str());
            return false;
        }

        // Get the entry script path
        std::string entryScriptPath = project->GetEntryScriptPath(executionPath);
        if (entryScriptPath.empty()) {
            Log::Error("[%s] No entry script found for project: %s",
                       m_Name.c_str(), project->GetName().c_str());
            return false;
        }

        Log::Info("[%s] Loading TAS script: %s",
                  m_Name.c_str(), entryScriptPath.c_str());

        // Load and execute the main script file in the Lua VM
        auto loadResult = m_LuaState.LoadFile(entryScriptPath);
        if (loadResult.IsError()) {
            Log::Error("[%s] Failed to load script: %s",
                       m_Name.c_str(), loadResult.GetError().message.c_str());
            CleanupCurrentProject();
            return false;
        }

        lua_State *L = m_LuaState.Get();
        auto execResult = tas::lua::ProtectedCall(L, 0, 0);
        if (execResult.IsError()) {
            Log::Error("[%s] Failed to execute script: %s",
                       m_Name.c_str(), execResult.GetError().message.c_str());
            CleanupCurrentProject();
            return false;
        }

        // The script should define a global 'main' function
        lua_getglobal(L, "main");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            Log::Error("[%s] 'main' function not found in entry script.",
                       m_Name.c_str());
            CleanupCurrentProject();
            return false;
        }
        tas::lua::LuaFunction mainFunc = tas::lua::LuaFunction::FromStack(L, -1);

        // Start the main coroutine
        if (m_Scheduler) {
            m_Scheduler->AddCoroutineTask(std::move(mainFunc));
        }

        // Set execution state
        m_CurrentProject = project;
        m_CurrentExecutionPath = executionPath;
        m_IsExecuting = true;
        m_IsPaused = false;

        NotifyStatusChange(true);

        Log::Info("[%s] TAS script '%s' loaded and started.",
                  m_Name.c_str(), project->GetName().c_str());
        return true;
    } catch (const std::exception &e) {
        Log::Error("[%s] Exception loading TAS script: %s",
                   m_Name.c_str(), e.what());
        CleanupCurrentProject();
        return false;
    }
}

void ScriptContext::Stop() {
    m_ThreadValidator.AssertOwnership();

    if (!m_IsExecuting) return;

    Log::Info("[%s] Stopping script execution...", m_Name.c_str());

    try {
        // Clear scheduler
        if (m_Scheduler) {
            m_Scheduler->Clear();
        }

        // Clear event listeners
        if (m_EventManager) {
            m_EventManager->ClearListeners();
        }

        ClearGameEventListeners();

        // Clean up project resources
        CleanupCurrentProject();
        m_CurrentProject = nullptr;
        m_CurrentExecutionPath.clear();

        // Reset execution state
        m_IsExecuting = false;
        m_IsPaused = false;

        NotifyStatusChange(false);

        Log::Info("[%s] Script execution stopped.", m_Name.c_str());
    } catch (const std::exception &e) {
        Log::Error("[%s] Exception stopping script execution: %s", m_Name.c_str(), e.what());
    }
}

void ScriptContext::Pause() {
    m_ThreadValidator.AssertOwnership();

    if (!m_IsExecuting || m_IsPaused) {
        return;
    }

    if (m_Scheduler) {
        m_Scheduler->Pause();
    }

    m_IsPaused = true;
    m_Sleeping = false;
    m_TicksSinceLastActive = 0;

    Log::Info("[%s] Script execution paused.", m_Name.c_str());
}

void ScriptContext::Resume() {
    m_ThreadValidator.AssertOwnership();

    if (!m_IsExecuting || !m_IsPaused) {
        return;
    }

    if (m_Scheduler) {
        m_Scheduler->Resume();
    }

    m_IsPaused = false;
    m_Sleeping = false;
    m_TicksSinceLastActive = 0;

    Log::Info("[%s] Script execution resumed.", m_Name.c_str());
}

void ScriptContext::Tick() {
    m_ThreadValidator.AssertOwnership();

    if (!m_IsExecuting || !m_Scheduler) {
        return;
    }

    if (m_IsPaused) {
        return;
    }

    // Handle sleep mode: only tick every N frames when sleeping
    if (m_Sleeping) {
        m_TicksSinceLastActive++;
        if (m_TicksSinceLastActive < m_SleepInterval) {
            return; // Skip this tick, still sleeping
        }
        // Time for a sleep-tick, reset counter
        m_TicksSinceLastActive = 0;
    }

    try {
        // Process Lua scheduler
        m_Scheduler->Tick();

        // Check if script execution has completed
        if (!m_Scheduler->IsRunning()) {
            Log::Info("[%s] Script execution completed naturally.", m_Name.c_str());
            m_IsExecuting = false;
            NotifyStatusChange(false);
        } else {
            // Check if context should go to sleep after this tick
            TrySleep();
        }
    } catch (const std::exception &e) {
        Log::Error("[%s] Exception during script tick: %s", m_Name.c_str(), e.what());
        m_IsExecuting = false;
        NotifyStatusChange(false);
    }
}

bool ScriptContext::IsExecuting() const {
    return m_IsExecuting && m_Scheduler && m_Scheduler->IsRunning();
}

ScriptContextManager *ScriptContext::GetScriptContextManager() const {
    return m_Engine->GetServiceProvider().Resolve<ScriptContextManager>();
}

size_t ScriptContext::GetCurrentTick() const {
    return m_Engine->GetCurrentTick();
}

size_t ScriptContext::AddGameEventListener(GameEventType eventType, tas::lua::LuaFunction callback) {
    m_ThreadValidator.AssertOwnership();

    if (!callback.IsValid()) {
        throw std::invalid_argument("events.on: callback must be a valid function");
    }

    auto &listeners = m_GameEventListeners[eventType];
    const bool wasEmpty = listeners.empty();

    const size_t listenerId = ++m_NextGameEventListenerId;
    listeners.push_back(GameEventListener{listenerId, std::move(callback)});

    if (wasEmpty) {
        if (auto *contextManager = GetScriptContextManager()) {
            contextManager->SubscribeToGameEvent(m_Name, eventType);
        }
    }

    return listenerId;
}

bool ScriptContext::RemoveGameEventListener(size_t listenerId) {
    m_ThreadValidator.AssertOwnership();

    for (auto it = m_GameEventListeners.begin(); it != m_GameEventListeners.end(); ++it) {
        auto &listeners = it->second;
        auto listenerIt = std::remove_if(
            listeners.begin(),
            listeners.end(),
            [listenerId](const GameEventListener &listener) {
                return listener.id == listenerId;
            });

        if (listenerIt == listeners.end()) {
            continue;
        }

        listeners.erase(listenerIt, listeners.end());
        if (listeners.empty()) {
            if (auto *contextManager = GetScriptContextManager()) {
                contextManager->UnsubscribeFromGameEvent(m_Name, it->first);
            }
            m_GameEventListeners.erase(it);
        }
        return true;
    }

    return false;
}

void ScriptContext::ClearGameEventListeners() {
    m_ThreadValidator.AssertOwnership();

    if (auto *contextManager = GetScriptContextManager()) {
        contextManager->UnsubscribeFromAllGameEvents(m_Name);
    }
    m_GameEventListeners.clear();
}

bool ScriptContext::HasGameEventListener(GameEventType eventType) const {
    auto it = m_GameEventListeners.find(eventType);
    return it != m_GameEventListeners.end() && !it->second.empty();
}

void ScriptContext::DispatchGameEvent(const LuaGameEvent &event) {
    if (!m_IsExecuting) {
        return;
    }

    // Wake tas.wait_event / tas.async.wait_for_event waits, which listen by name.
    if (m_EventManager && event.name && *event.name) {
        m_EventManager->FireEvent(event.name);
    }

    DispatchToLuaListeners(event);

    auto it = m_GameEventListeners.find(event.type);
    if (it == m_GameEventListeners.end() || it->second.empty()) {
        return;
    }

    try {
        tas::lua::LuaValue eventValue = MakeGameEventValue(event);

        for (const auto &listener : it->second) {
            if (!listener.callback.IsValid()) {
                continue;
            }
            auto result = listener.callback.Call(1, 0, [&](lua_State *state) {
                eventValue.Push(state);
            });
            if (result.IsError()) {
                Log::Error("[%s] Exception dispatching typed game event: %s",
                           m_Name.c_str(), result.GetError().message.c_str());
            }
        }
    } catch (const std::exception &e) {
        Log::Error("[%s] Exception dispatching typed game event: %s", m_Name.c_str(), e.what());
    }
}

void ScriptContext::DispatchToLuaListeners(const LuaGameEvent &event) {
    lua_State *state = m_LuaState.Get();
    if (!state || !event.name || !*event.name) {
        return;
    }

    tas::lua::LuaStackGuard guard(state);
    if (lua_getfield(state, LUA_REGISTRYINDEX, LuaApi::kEventDispatchKey) != LUA_TFUNCTION) {
        return;
    }
    lua_pushstring(state, event.name);
    MakeGameEventValue(event).Push(state);
    if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
        const char *message = lua_tostring(state, -1);
        Log::Error("[%s] Exception dispatching event '%s' to tas.on listeners: %s",
                   m_Name.c_str(), event.name, message ? message : "unknown error");
    }
}

std::string ScriptContext::PrepareProjectForExecution(TASProject *project) {
    if (!project || !project->IsScriptProject()) {
        return "";
    }

    // For zip projects, we need to prepare them for execution (extract if needed)
    if (project->IsZipProject()) {
        auto *projectManager = GetProjectManager();
        if (!projectManager) {
            Log::Error("[%s] ProjectManager not available for zip project preparation.",
                       m_Name.c_str());
            return "";
        }

        std::string executionPath = projectManager->PrepareProjectForExecution(project);
        if (executionPath.empty()) {
            Log::Error("[%s] Failed to prepare zip project for execution: %s",
                       m_Name.c_str(), project->GetName().c_str());
            return "";
        }

        // Update the project's execution base path for script resolution
        project->SetExecutionBasePath(executionPath);

        Log::Info("[%s] Zip project prepared for execution: %s -> %s",
                  m_Name.c_str(), project->GetPath().c_str(), executionPath.c_str());
        return executionPath;
    } else {
        // For directory projects, use the project path directly
        return project->GetPath();
    }
}

void ScriptContext::CleanupCurrentProject() {
    if (!m_CurrentProject) {
        return;
    }

    // Clean up temporary directories for zip projects
    if (m_CurrentProject->IsZipProject() && GetProjectManager()) {
        GetProjectManager()->CleanupProjectTempDirectory(m_CurrentProject);
        m_CurrentProject->SetExecutionBasePath(""); // Clear execution base path
    }

    m_CurrentProject = nullptr;
    m_CurrentExecutionPath.clear();
}

ProjectManager *ScriptContext::GetProjectManager() const {
    return m_Engine->GetServiceProvider().Resolve<ProjectManager>();
}

RecordPlayer *ScriptContext::GetRecordPlayer() const {
    return m_Engine->GetServiceProvider().Resolve<RecordPlayer>();
}

GameInterface *ScriptContext::GetGameInterface() const {
    return m_Engine->GetGameInterface();
}

SavestateManager *ScriptContext::GetSavestateManager() const {
    return m_Engine->GetServiceProvider().Resolve<SavestateManager>();
}

ValidationService *ScriptContext::GetValidationService() const {
    return m_Engine->GetServiceProvider().Resolve<ValidationService>();
}

// ============================================================================
// GC Mode Management
// ============================================================================

bool ScriptContext::SetGCMode(LuaGCMode mode) {
    if (!m_IsInitialized && !m_LuaState.Get()) {
        Log::Warn("[%s] Cannot set GC mode: context not initialized.", m_Name.c_str());
        return false;
    }

    try {
        lua_State *L = m_LuaState.Get();

        // STACK SAFETY: lua_gc() does not manipulate the Lua stack, so no stack guard needed.
        // However, we record the stack top for debug validation.
#ifdef _DEBUG
        int stackTop = lua_gettop(L);
#endif

        // Lua 5.4+ GC modes: LUA_GCGEN (generational), LUA_GCINC (incremental)
        // Note: Check Lua version and availability
#if LUA_VERSION_NUM >= 504
        if (mode == LuaGCMode::Generational) {
            // Switch to generational mode
            lua_gc(L, LUA_GCGEN, 0, 0);
            m_GCMode = LuaGCMode::Generational;
            Log::Info("[%s] GC mode set to Generational.", m_Name.c_str());
        } else {
            // Switch to incremental mode
            lua_gc(L, LUA_GCINC, 0, 0, 0);
            m_GCMode = LuaGCMode::Incremental;
            Log::Info("[%s] GC mode set to Incremental.", m_Name.c_str());
        }

        // DEBUG: Verify stack balance
#ifdef _DEBUG
        int stackTopAfter = lua_gettop(L);
        if (stackTop != stackTopAfter) {
            Log::Error("[%s] STACK IMBALANCE in SetGCMode: before=%d, after=%d",
                       m_Name.c_str(), stackTop, stackTopAfter);
        }
#endif

        return true;
#else
        // Lua 5.3 or earlier - only incremental GC available
        m_GCMode = LuaGCMode::Incremental;
        Log::Warn("[%s] Lua version < 5.4: only incremental GC available.", m_Name.c_str());

        // DEBUG: Verify stack balance
#ifdef _DEBUG
        int stackTopAfter = lua_gettop(L);
        if (stackTop != stackTopAfter) {
            Log::Error("[%s] STACK IMBALANCE in SetGCMode: before=%d, after=%d",
                       m_Name.c_str(), stackTop, stackTopAfter);
        }
#endif

        return false;
#endif
    } catch (const std::exception &e) {
        Log::Error("[%s] Failed to set GC mode: %s", m_Name.c_str(), e.what());
        // NOTE: If an exception occurs, the stack should still be balanced since lua_gc()
        // doesn't push/pop values. But we log the error to be safe.
        return false;
    }
}

// ============================================================================
// Memory Monitoring
// ============================================================================

size_t ScriptContext::GetLuaMemoryBytes() const {
    if (!m_IsInitialized) {
        return 0;
    }

    try {
        // Get memory usage via collectgarbage("count") which returns KB
        lua_State *L = m_LuaState.Get();
        int kb = lua_gc(L, LUA_GCCOUNT, 0);
        return static_cast<size_t>(kb) * 1024;
    } catch (const std::exception &) {
        return 0;
    }
}

double ScriptContext::GetLuaMemoryKB() const {
    if (!m_IsInitialized) {
        return 0.0;
    }

    try {
        lua_State *L = m_LuaState.Get();
        int kb = lua_gc(L, LUA_GCCOUNT, 0);
        return static_cast<double>(kb);
    } catch (const std::exception &) {
        return 0.0;
    }
}

// ============================================================================
// Sleep/Idle Management
// ============================================================================

bool ScriptContext::ShouldSleep() const {
    if (!m_IsExecuting) {
        return true; // Not executing, can sleep
    }

    // Check if scheduler has active tasks
    if (m_Scheduler && m_Scheduler->IsRunning()) {
        return false; // Has active coroutines, don't sleep
    }

    // Note: Message delivery and event callbacks wake the context by scheduling work
    // through the scheduler, so checking scheduler state is sufficient for sleep detection.
    // The ScriptContextManager is responsible for waking sleeping contexts when:
    // - New messages arrive (delivered via scheduler)
    // - Events are fired (callbacks scheduled via scheduler)
    // - Shared data watches trigger (callbacks scheduled via scheduler)

    // If no active tasks, can sleep
    return true;
}

void ScriptContext::TrySleep() {
    if (ShouldSleep() && !m_Sleeping) {
        m_Sleeping = true;
        m_TicksSinceLastActive = 0;
        Log::Info("[%s] Context entering sleep mode.", m_Name.c_str());
    }
}
