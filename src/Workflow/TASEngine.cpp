#include "TASEngine.h"

#include <filesystem>
#include <cstdlib>
#include <cerrno>
#include <limits>

#include "EngineBootstrap.h"
#include "Runtime/ContextLifecycleCoordinator.h"
#include "EventManager.h"
#include "GameEvents.h"
#include "GameInterface.h"
#include "HookManager.h"
#include "InputSystem.h"
#include "Logger.h"
#include "Runtime/LuaTypedEventBridge.h"
#include "PlaybackService.h"
#include "ProjectManager.h"
#include "RecordPlayer.h"
#include "Recorder.h"
#include "RecordingService.h"
#include "Runtime/RuntimeSession.h"
#include "Runtime/RuntimeEventRouter.h"
#include "SavestateManager.h"
#include "ScriptContext.h"
#include "ScriptContextManager.h"
#include "ScriptGenerator.h"
#include "StartupProjectManager.h"
#include "TASProject.h"
#include "TranslationService.h"
#include "ValidationService.h"

#ifdef ENABLE_REPL
#include "LuaREPLServer.h"
#endif

namespace fs = std::filesystem;

TASEngine::TASEngine(GameInterface *gameInterface, EventBus *eventBus, HookManager *hookManager)
    : m_GameInterface(gameInterface),
      m_EventBus(eventBus),
      m_HookManager(hookManager) {
    if (!m_GameInterface || !m_EventBus || !m_HookManager) {
        throw std::runtime_error("TASEngine requires valid game and runtime infrastructure.");
    }
}

TASEngine::~TASEngine() {
    if (!m_ShuttingDown) {
        Shutdown();
    }
}

bool TASEngine::Initialize() {
    if (m_ShuttingDown) {
        Log::Error("Cannot initialize TASEngine during shutdown.");
        return false;
    }

    if (!EngineBootstrap::InitializeCoreSubsystems(*this)) {
        return false;
    }

    if (!EngineBootstrap::InitializeHighLevelSubsystems(*this)) {
        return false;
    }

    Log::Info("TASEngine initialization complete.");
    return true;
}

void TASEngine::Shutdown() {
    const bool wasShuttingDown = m_ShuttingDown.exchange(true);
    if (wasShuttingDown) {
        return;
    }

    bool handledBySession = false;
    if (auto *session = Session(); session && !session->IsShuttingDown()) {
        auto result = session->Shutdown();
        if (!result.IsOk()) {
            Log::Error("Failed to transition to shutdown state: %s",
                       result.GetError().message.c_str());
        } else {
            handledBySession = true;
        }
    }

    if (!handledBySession) {
        auto *validationService = m_ServiceContainer.Resolve<ValidationService>();
        auto *playbackService = m_ServiceContainer.Resolve<PlaybackService>();
        auto *recordingService = m_ServiceContainer.Resolve<RecordingService>();
        auto *translationService = m_ServiceContainer.Resolve<TranslationService>();

        if (validationService && validationService->IsActive()) {
            validationService->StopImmediate();
        }
        if (playbackService) {
            playbackService->StopPlaybackImmediate();
        }
        if (recordingService) {
            recordingService->StopRecordingImmediate();
        }
        if (translationService) {
            translationService->StopTranslationImmediate();
        }
    }

    auto *router = m_ServiceContainer.Resolve<RuntimeEventRouter>();
    if (router) {
        router->Shutdown();
    }
    auto *bridge = m_ServiceContainer.Resolve<LuaTypedEventBridge>();
    if (bridge) {
        bridge->Shutdown();
    }

#ifdef ENABLE_REPL
    auto *replServer = m_ServiceContainer.Resolve<LuaREPLServer>();
    if (replServer) {
        replServer->Shutdown();
    }
#endif

    auto *scriptContextManager = m_ServiceContainer.Resolve<ScriptContextManager>();
    if (scriptContextManager) {
        scriptContextManager->Shutdown();
    }

    m_ServiceContainer.Clear();

    m_GameInterface = nullptr;
    Log::Info("TASEngine shutdown complete.");
}

void TASEngine::Start() {
    if (m_ShuttingDown || !m_GameInterface) {
        return;
    }

    const float deterministicDeltaTime = ResolveLevelLoadPhysicsDeltaTime();
    const bool tasActive = LevelLoadRunsTAS();
    const bool preserveHistory = m_PreserveLevelHistory;
    auto resetPhysicsForLevel = [this, deterministicDeltaTime, tasActive, preserveHistory]() {
        if (m_GameInterface) {
            if (deterministicDeltaTime > 0.0f) {
                Log::Info("Resetting physics time with TAS delta %.6f ms.", deterministicDeltaTime);
                m_GameInterface->ResetPhysicsTime(deterministicDeltaTime);
            } else {
                m_GameInterface->ResetPhysicsTime();
            }
            // The first level load after the game starts sees the physics state
            // of a fresh process; later loads inherit it from earlier levels.
            const auto current = m_GameInterface->CapturePhysicsLevelBaseline();
            if (!m_PhysicsLevelBaseline) {
                m_PhysicsLevelBaseline = current;
                Log::Info("Physics level baseline: time factor %g, next_movement_check %d, seed %d.",
                          current.timeFactor, current.nextMovementCheck, current.randomSeed);
                if (current.hasGravity)
                    Log::Info("Physics level baseline gravity: %.17g, %.17g, %.17g.",
                              current.gravity[0], current.gravity[1], current.gravity[2]);
            }
            if (tasActive && preserveHistory) {
                Log::Info("Preserving inherited physics level state: time factor %.9g, next_movement_check %d, seed %d.",
                          current.timeFactor, current.nextMovementCheck, current.randomSeed);
            } else if (tasActive) {
                auto baseline = *m_PhysicsLevelBaseline;
                // Diagnostic only: test an independently captured original
                // scheduler baseline. Never infer it from a later trajectory.
                const char *seedText = std::getenv("BALLANCE_TAS_DIAG_LEVEL_SEED");
                const char *checkText = std::getenv("BALLANCE_TAS_DIAG_LEVEL_MOVEMENT_CHECK");
                if (seedText || checkText) {
                    auto parse = [](const char *text, long long low, long long high, long long &out) {
                        if (!text || !*text) return false;
                        char *end = nullptr;
                        errno = 0;
                        out = std::strtoll(text, &end, 10);
                        return errno != ERANGE && end != text && *end == '\0' && out >= low && out <= high;
                    };
                    long long seed = 0, check = 0;
                    if (parse(seedText, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), seed)
                        && seed != 0 && parse(checkText, 1, std::numeric_limits<short>::max(), check)) {
                        baseline.randomSeed = static_cast<int>(seed);
                        baseline.nextMovementCheck = static_cast<short>(check);
                        Log::Info("Diagnostic scheduler baseline: seed %d, next_movement_check %d.",
                                  baseline.randomSeed, baseline.nextMovementCheck);
                    } else {
                        Log::Error("Invalid diagnostic scheduler baseline; both seed and positive countdown are required.");
                    }
                }
                Log::Info("Restoring physics level baseline (was time factor %g, next_movement_check %d, seed %d).",
                          current.timeFactor, current.nextMovementCheck, current.randomSeed);
                if (current.hasGravity)
                    Log::Info("Restoring physics level gravity (was %.17g, %.17g, %.17g).",
                              current.gravity[0], current.gravity[1], current.gravity[2]);
                m_GameInterface->RestorePhysicsLevelBaseline(baseline);
            }
        }
    };

    const char *deferredReset = std::getenv("BALLANCE_TAS_DEFER_LEVEL_CLOCK_RESET");
    if (tasActive && deferredReset && std::strcmp(deferredReset, "sync") == 0) {
        // Retain the old synchronous intervention only as an explicit
        // diagnostic control. It changes which pre-load bodies simulate.
        resetPhysicsForLevel();
    } else {
        // Original TASSupport::OnStart and upstream TASEngine::Start both use
        // AddTimer(1). Preserve that callback phase even when TAS is active:
        // synchronous reset gives the pre-load physics a different history.
        // Diagnostic zero selects this frame's timer phase; one or no override
        // uses the original delay. A timer tick is not elapsed physics time.
        const size_t delay = tasActive && deferredReset &&
            deferredReset[0] == '0' && deferredReset[1] == '\0' ? 0ul : 1ul;
        AddTimer(delay, std::move(resetPhysicsForLevel));
    }
}

void TASEngine::Stop() {
    auto *session = Session();
    if (m_ShuttingDown || !session || session->IsIdle() || session->IsShuttingDown()) {
        return;
    }
    // An armed TAS is not running in this level; it keeps waiting for its next load.
    if (session->IsPending()) {
        return;
    }

    bool shouldKeepGlobalScript = false;
    auto *playbackService = m_ServiceContainer.Resolve<PlaybackService>();
    if (IsPlayingScript() && playbackService) {
        auto *project = playbackService->GetCurrentProject();
        shouldKeepGlobalScript = project && project->IsGlobalProject();
    }

    if (shouldKeepGlobalScript) {
        auto *validationService = m_ServiceContainer.Resolve<ValidationService>();
        if (validationService && validationService->IsActive()) {
            StopValidationRecording();
        }
        Log::Info("Keeping global script playback active during level transition.");
        return;
    }

    m_Requests.clearProjectOnStop = true;
    auto result = session->OnLevelEnd({true});
    const bool transitioned = result.IsOk();
    if (!result.IsOk()) {
        Log::Error("Runtime session transition failed for level end: %s",
                   result.GetError().message.c_str());
    }
    if (transitioned && IsAutoRestartEnabled()) {
        RestartCurrentProject();
    }
}

bool TASEngine::StartRecording() {
    auto *session = Session();
    if (m_ShuttingDown || !session || !session->IsIdle()) {
        Log::Warn("Cannot start recording: TAS is not idle.");
        return false;
    }

    ClearControlRequests();
    m_LastCompletedPlaybackProjectName.clear();
    m_Requests.requestedValidationRecording = false;
    auto result = session->StartRecording({false});
    if (!result.IsOk()) {
        Log::Error("Runtime session failed to start recording: %s",
                   result.GetError().message.c_str());
        return false;
    }
    return true;
}

void TASEngine::StopRecording() {
    auto *recordingService = m_ServiceContainer.Resolve<RecordingService>();
    if (m_ShuttingDown) {
        if (recordingService) {
            recordingService->StopRecordingImmediate();
        }
        return;
    }

    auto *session = Session();
    if (!session || (!session->IsRecording() && !session->IsPendingRecord())) {
        return;
    }

    m_Requests.clearProjectOnStop = false;
    StopSession({false}, "stop recording");
}

size_t TASEngine::GetRecordingFrameCount() const {
    auto *recordingService = m_ServiceContainer.Resolve<RecordingService>();
    return recordingService ? recordingService->GetFrameCount() : 0;
}

bool TASEngine::StartReplay() {
    return StartReplay(ReplayValidation{});
}

bool TASEngine::StartReplay(const ReplayValidation &validation) {
    auto *session = Session();
    if (m_ShuttingDown || !session || !session->IsIdle()) {
        Log::Warn("Cannot start replay: TAS is not idle.");
        return false;
    }

    auto *projectManager = m_ServiceContainer.Resolve<ProjectManager>();
    if (!projectManager) {
        Log::Error("ProjectManager not available.");
        return false;
    }

    TASProject *project = projectManager->GetCurrentProject();
    if (!project || !project->IsValid()) {
        Log::Error("No valid TAS project selected.");
        return false;
    }

    PlaybackType type = PlaybackType::None;
    if (project->IsScriptProject()) {
        type = PlaybackType::Script;
    } else if (project->IsRecordProject()) {
        type = PlaybackType::Record;
    }

    if (type == PlaybackType::None) {
        Log::Error("Unable to determine playback type for project: %s", project->GetName().c_str());
        return false;
    }

    ClearControlRequests();
    m_LastCompletedPlaybackProjectName.clear();
    m_Requests.requestedProject = project;
    m_Requests.requestedPlaybackType = type;
    m_Requests.validationReference = validation.reference;
    m_Requests.validationExact = validation.exact;

    auto result = session->StartPlayback(project, type, {m_ValidationEnabled || validation.enabled});
    if (!result.IsOk()) {
        Log::Error("Runtime session failed to start replay: %s",
                   result.GetError().message.c_str());
        return false;
    }
    return true;
}

void TASEngine::StopReplay(bool clearProject) {
    auto *playbackService = m_ServiceContainer.Resolve<PlaybackService>();
    if (m_ShuttingDown) {
        auto *validationService = m_ServiceContainer.Resolve<ValidationService>();
        if (validationService && validationService->IsActive()) {
            validationService->StopImmediate();
        }
        if (playbackService) {
            playbackService->StopPlaybackImmediate();
        }
        return;
    }

    auto *session = Session();
    if (!session ||
        (!session->IsPlaying() && !session->IsPaused() && !session->IsPendingPlay())) {
        return;
    }

    m_Requests.clearProjectOnStop = clearProject;
    m_LastCompletedPlaybackProjectName.clear();
    StopSession({clearProject}, "stop replay");
}

bool TASEngine::StartTranslation() {
    auto *session = Session();
    if (m_ShuttingDown || !session || !session->IsIdle()) {
        Log::Warn("Cannot start translation: TAS is not idle.");
        return false;
    }

    auto *projectManager = m_ServiceContainer.Resolve<ProjectManager>();
    if (!projectManager) {
        Log::Error("ProjectManager not available.");
        return false;
    }

    TASProject *project = projectManager->GetCurrentProject();
    if (!project) {
        Log::Error("No project selected for translation.");
        return false;
    }

    ClearControlRequests();
    m_LastCompletedPlaybackProjectName.clear();
    m_Requests.requestedProject = project;

    auto result = session->StartTranslation(project, {});
    if (!result.IsOk()) {
        Log::Error("Runtime session failed to start translation: %s",
                   result.GetError().message.c_str());
        return false;
    }
    return true;
}

void TASEngine::StopTranslation(bool clearProject) {
    auto *translationService = m_ServiceContainer.Resolve<TranslationService>();
    if (m_ShuttingDown) {
        if (translationService) {
            translationService->StopTranslationImmediate();
        }
        return;
    }

    auto *session = Session();
    if (!session ||
        (!session->IsTranslating() && !session->IsPendingTranslate())) {
        return;
    }

    m_Requests.clearProjectOnStop = clearProject;
    StopSession({clearProject}, "stop translation");
}

bool TASEngine::StartValidationRecording(const std::string &outputPath) {
    auto *validationService = m_ServiceContainer.Resolve<ValidationService>();
    auto *playbackService = m_ServiceContainer.Resolve<PlaybackService>();
    if (!validationService || !playbackService) {
        return false;
    }

    if (!playbackService->IsPlaying()) {
        Log::Error("Validation recording requires active playback.");
        return false;
    }

    auto result = validationService->Start(outputPath);
    if (!result.IsOk()) {
        Log::Error("Validation recording: %s", result.GetError().message.c_str());
        return false;
    }
    return true;
}

bool TASEngine::StopValidationRecording() {
    auto *validationService = m_ServiceContainer.Resolve<ValidationService>();
    if (!validationService || !validationService->IsActive()) {
        return false;
    }

    auto result = validationService->Stop();
    if (!result.IsOk()) {
        Log::Error("Validation recording stop: %s", result.GetError().message.c_str());
        return false;
    }
    return true;
}

bool TASEngine::IsValidationEnabled() const {
    return m_ValidationEnabled;
}

void TASEngine::SetValidationEnabled(bool enabled) {
    m_ValidationEnabled = enabled;
}

const std::string &TASEngine::GetValidationOutputPath() const {
    static const std::string empty;
    auto *validationService = m_ServiceContainer.Resolve<ValidationService>();
    return validationService ? validationService->GetOutputPath() : empty;
}

std::string TASEngine::GetLastTranslationResultMessage() const {
    auto *translationService = m_ServiceContainer.Resolve<TranslationService>();
    return translationService ? translationService->GetLastResultMessage() : std::string{};
}

bool TASEngine::RestartCurrentProject() {
    auto *projectManager = m_ServiceContainer.Resolve<ProjectManager>();
    if (m_ShuttingDown || !projectManager) {
        Log::Warn("Cannot restart during shutdown or without ProjectManager.");
        return false;
    }

    TASProject *project = projectManager->GetCurrentProject();
    if (!project || !project->IsValid()) {
        Log::Error("No valid project to restart.");
        return false;
    }

    const int level = m_GameInterface ? m_GameInterface->GetCurrentLevel() : 0;
    if (!StartReplay()) {
        return false;
    }
    m_Requests.restartLevel = level > 0 ? level : 0;
    if (m_Requests.restartLevel > 0) {
        Log::Info("Restarting TAS project: %s (plays when Level %d loads again).",
                  project->GetName().c_str(), m_Requests.restartLevel);
    } else {
        Log::Info("Restarting TAS project: %s", project->GetName().c_str());
    }
    return true;
}

bool TASEngine::LevelLoadRunsTAS() const {
    const auto *session = Session();
    if (m_ShuttingDown || !session || session->IsIdle() || session->IsShuttingDown()) {
        return false;
    }
    // The restarted level is only passed through on the way to the reload.
    if (m_LevelReload != LevelReload::None) {
        return false;
    }
    if (session->IsPending() && m_Requests.restartLevel > 0 && m_GameInterface &&
        m_GameInterface->GetCurrentLevel() != m_Requests.restartLevel) {
        return false;
    }
    return true;
}

void TASEngine::ReloadInsteadOfRestart() {
    const auto *session = Session();
    if (!m_GameInterface || !session || !session->IsPending() || !LevelLoadRunsTAS()) {
        return;
    }
    const int level = m_GameInterface->GetCurrentLevel();
    if (level <= 0) {
        return;
    }
    m_ReloadLevel = level;
    m_LevelReload = LevelReload::AwaitStart;
    m_ReloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    Log::Info("Level %d restarted with a TAS armed; loading it again, as the TAS starts with a level load.",
              level);
}

void TASEngine::OnLevelStartedForReload() {
    if (m_LevelReload != LevelReload::AwaitStart) {
        return;
    }
    std::string error;
    if (!m_GameInterface->SendGameFlowEvent(GameInterface::GameFlowEvent::ExitLevel, error)) {
        CancelLevelReload(error.c_str());
        return;
    }
    m_LevelReload = LevelReload::AwaitMenu;
    m_ReloadMenuFrames = 0;
}

void TASEngine::TickLevelReload() {
    if (m_LevelReload == LevelReload::None) {
        return;
    }
    const auto *session = Session();
    if (!session || !session->IsPending()) {
        // Disarmed meanwhile: what is left is an ordinary restart or exit.
        m_LevelReload = LevelReload::None;
        Log::Info("TAS disarmed; Level %d is not loaded again.", m_ReloadLevel);
        return;
    }
    if (std::chrono::steady_clock::now() > m_ReloadDeadline) {
        CancelLevelReload("timed out");
        return;
    }
    if (m_LevelReload != LevelReload::AwaitMenu) {
        return;
    }
    // Menus take a couple of frames to switch their buttons on.
    if (m_GameInterface->GetActiveMenu() != "main") {
        m_ReloadMenuFrames = 0;
        return;
    }
    if (++m_ReloadMenuFrames < 3) {
        return;
    }
    m_LevelReload = LevelReload::None;
    std::string error;
    if (!m_GameInterface->LoadLevel(m_ReloadLevel, error)) {
        Log::Warn("Could not load Level %d again after the restart: %s", m_ReloadLevel, error.c_str());
    }
}

void TASEngine::CancelLevelReload(const char *reason) {
    Log::Warn("Could not load Level %d again after the restart (%s); the TAS stays armed for its next load.",
              m_ReloadLevel, reason);
    m_LevelReload = LevelReload::None;
}

bool TASEngine::IsPlaying() const {
    auto *session = Session();
    return session && session->IsPlaying();
}

bool TASEngine::IsRecording() const {
    auto *session = Session();
    return session && session->IsRecording();
}

bool TASEngine::IsTranslating() const {
    auto *session = Session();
    return session && session->IsTranslating();
}

bool TASEngine::IsIdle() const {
    auto *session = Session();
    return session && session->IsIdle();
}

bool TASEngine::IsPaused() const {
    auto *session = Session();
    return session && session->IsPaused();
}

bool TASEngine::IsPlayingScript() const {
    return (IsPlaying() || IsPaused()) && GetPlaybackType() == PlaybackType::Script;
}

bool TASEngine::DrivesScriptContexts() const {
    if (IsPlayingScript()) {
        return true;
    }
    auto *translation = m_ServiceContainer.Resolve<TranslationService>();
    return translation && translation->IsTranslating() &&
           translation->GetDirection() == TranslationDirection::ScriptToRecord;
}

bool TASEngine::IsPlayingRecord() const {
    return (IsPlaying() || IsPaused()) && GetPlaybackType() == PlaybackType::Record;
}

PlaybackType TASEngine::GetPlaybackType() const {
    auto *session = Session();
    return session ? session->GetPlaybackType() : PlaybackType::None;
}

bool TASEngine::IsPendingPlay() const {
    auto *session = Session();
    return session && session->IsPendingPlay();
}

bool TASEngine::IsPendingRecord() const {
    auto *session = Session();
    return session && session->IsPendingRecord();
}

bool TASEngine::IsPendingTranslate() const {
    auto *session = Session();
    return session && session->IsPendingTranslate();
}

RuntimeSession *TASEngine::Session() const {
    return m_ServiceContainer.Resolve<RuntimeSession>();
}

bool TASEngine::StopSession(RuntimeSession::StopOptions options, const char *reason) {
    auto *session = Session();
    if (!session) {
        Log::Error("Runtime session not available for %s.", reason ? reason : "transition");
        return false;
    }

    auto result = session->Stop(options);
    if (!result.IsOk()) {
        Log::Error("Runtime session transition failed for %s: %s",
                   reason ? reason : "transition",
                   result.GetError().message.c_str());
        return false;
    }

    return true;
}

// Runs after PlaybackCompletedEvent, whose router has usually stopped the session already.
void TASEngine::HandlePlaybackCompleted() {
    auto *session = Session();
    if (m_ShuttingDown || !session) {
        return;
    }

    // Global scripts run on in their context; everything else can be armed again.
    bool canRestart = false;
    if (auto *playbackService = m_ServiceContainer.Resolve<PlaybackService>()) {
        if (auto *project = playbackService->GetCurrentProject()) {
            m_LastCompletedPlaybackProjectName = project->GetName();
            canRestart = !(project->IsScriptProject() && project->IsGlobalProject());
        }
    }

    if (session->IsPlaying() || session->IsPaused()) {
        m_Requests.clearProjectOnStop = false;
        auto result = session->OnPlaybackCompleted(GetPlaybackType());
        if (!result.IsOk()) {
            Log::Error("Runtime session failed to complete playback: %s",
                       result.GetError().message.c_str());
            return;
        }
    }

    // Like a playback the level end stopped: scripts usually finish before the level
    // does, records usually after. Next frame, once the playback service has unwound.
    if (canRestart && IsAutoRestartEnabled()) {
        AddTimer(1ul, [this]() {
            const auto *current = Session();
            if (!m_ShuttingDown && current && current->IsIdle()) {
                RestartCurrentProject();
            }
        });
    }
}

float TASEngine::ResolveLevelLoadPhysicsDeltaTime() const {
    const auto *session = Session();
    if (!session || !LevelLoadRunsTAS()) {
        return 0.0f;
    }

    if (m_Requests.requestedProject && m_Requests.requestedProject->IsValid()) {
        return m_Requests.requestedProject->GetDeltaTime();
    }

    if (auto *playbackService = m_ServiceContainer.Resolve<PlaybackService>()) {
        if (auto *project = playbackService->GetCurrentProject();
            project && project->IsValid()) {
            return project->GetDeltaTime();
        }
    }

    if (session->IsPendingRecord() || session->IsRecording()) {
        const auto *recorder = m_ServiceContainer.Resolve<Recorder>();
        return recorder ? recorder->GetDeltaTime() : 0.0f;
    }

    if (session->IsPendingTranslate() || session->IsTranslating()) {
        const auto *recorder = m_ServiceContainer.Resolve<Recorder>();
        return recorder ? recorder->GetDeltaTime() : 0.0f;
    }

    return 0.0f;
}

std::string TASEngine::BuildValidationOutputPath(TASProject *project) const {
    if (!project) {
        return {};
    }

    fs::path path(project->GetPath());
    if (path.has_extension()) {
        path = path.parent_path();
    }

    if (path.empty()) {
        return {};
    }

    std::string outputPath = path.string();
    if (!outputPath.empty()) outputPath.push_back(fs::path::preferred_separator);
    return outputPath;
}

void TASEngine::ClearControlRequests() {
    m_Requests.Clear();
}

void TASEngine::AddTimer(size_t tick, const std::function<void()> &callback) {
    if (m_GameInterface) {
        m_GameInterface->AddTimer(tick, callback);
    }
}

lua_State *TASEngine::GetLuaState() const {
    auto *scriptContextManager = m_ServiceContainer.Resolve<ScriptContextManager>();
    if (!scriptContextManager) {
        return nullptr;
    }

    auto ctx = scriptContextManager->GetContext("global");
    return ctx ? ctx->GetLuaState().Get() : nullptr;
}

LuaScheduler *TASEngine::GetScheduler() const {
    auto *scriptContextManager = m_ServiceContainer.Resolve<ScriptContextManager>();
    if (!scriptContextManager) {
        return nullptr;
    }

    auto ctx = scriptContextManager->GetContext("global");
    return ctx ? ctx->GetScheduler() : nullptr;
}

size_t TASEngine::GetCurrentTick() const {
    if (auto *playbackService = m_ServiceContainer.Resolve<PlaybackService>()) {
        if (playbackService->IsPlaying() || playbackService->IsPaused()) {
            return playbackService->GetCurrentTick();
        }
    }

    if (auto *recordingService = m_ServiceContainer.Resolve<RecordingService>()) {
        if (recordingService->IsRecording()) {
            return recordingService->GetCurrentTick();
        }
    }

    if (auto *translationService = m_ServiceContainer.Resolve<TranslationService>()) {
        if (translationService->IsTranslating()) {
            return translationService->GetCurrentTick();
        }
    }

    return m_CurrentTick;
}

void TASEngine::SetCurrentTick(size_t tick) {
    m_CurrentTick = tick;
}
