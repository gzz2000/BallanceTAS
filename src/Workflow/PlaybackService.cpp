/**
 * @file PlaybackService.cpp
 * @brief Implementation of PlaybackService - unified script/record playback lifecycle.
 */

#include "PlaybackService.h"

#include "ServiceContainer.h"
#include "GameInterface.h"
#include "GameEvents.h"
#include "IGameControl.h"
#include "IInputAccess.h"
#include "ScriptContextManager.h"
#include "ScriptContext.h"
#include "RecordPlayer.h"
#include "ScriptFrameTime.h"
#include "InputSystem.h"
#include "Logger.h"
#include "TASProject.h"

#ifdef ENABLE_REPL
#include "LuaREPLServer.h"
#endif

#include <CKInputManager.h>
#include <CKTimeManager.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void TraceRecordPhase(const char *phase, size_t logicalTick,
                      RecordPlayer *recordPlayer, GameInterface *game,
                      CKInputManager *inputManager) {
    const char *path = std::getenv("BALLANCE_TAS_PHASE_TRACE");
    if (!path || !*path || (logicalTick >= 160 && (logicalTick < 920 || logicalTick > 950))) {
        return;
    }
    FILE *stream = std::fopen(path, "a");
    if (!stream) {
        return;
    }
    CKTimeManager *time = game ? game->GetTimeManager() : nullptr;
    CK3dEntity *ball = game ? game->GetActiveBall() : nullptr;
    const bool physical = game && ball && game->GetPhysicsObject(ball);
    VxVector position{};
    if (ball) {
        ball->GetPosition(&position);
    }
    const unsigned char *keys = inputManager ? inputManager->GetKeyboardState() : nullptr;
    std::fprintf(stream,
                 "%s host=%u logical=%zu record=%zu dt=%.9g score=%.9g "
                 "ball=%u physical=%d pos=(%.9g %.9g %.9g) keys=(%u %u %u %u)\n",
                 phase, time ? static_cast<unsigned>(time->GetMainTickCount()) : 0u,
                 logicalTick, recordPlayer ? recordPlayer->GetCurrentFrame() : 0u,
                 time ? time->GetLastDeltaTime() : 0.0f,
                 game ? game->GetSRScore() : 0.0f,
                 ball ? static_cast<unsigned>(ball->GetID()) : 0u, physical ? 1 : 0,
                 position.x, position.y, position.z,
                 keys ? static_cast<unsigned>(keys[CKKEY_UP]) : 0u,
                 keys ? static_cast<unsigned>(keys[CKKEY_DOWN]) : 0u,
                 keys ? static_cast<unsigned>(keys[CKKEY_LEFT]) : 0u,
                 keys ? static_cast<unsigned>(keys[CKKEY_RIGHT]) : 0u);
    std::fclose(stream);
}

void SetHostFastForward(bool enabled) {
#ifdef _WIN32
    _putenv_s("BALLANCE_FAST_FORWARD_ACTIVE", enabled ? "1" : "");
#else
    if (enabled)
        setenv("BALLANCE_FAST_FORWARD_ACTIVE", "1", 1);
    else
        unsetenv("BALLANCE_FAST_FORWARD_ACTIVE");
#endif
}
}

static std::string ResolvePlaybackLevelName(const TASProject *project, GameInterface *game) {
    if (!project) {
        return {};
    }

    return ScriptContextManager::ResolveLevelKey(
        project->GetTargetLevel(),
        game ? game->GetMapName() : "",
        game ? game->GetCurrentLevel() : 0);
}

PlaybackService::PlaybackService(ServiceProvider *provider)
    : m_ServiceProvider(provider) {
    if (!m_ServiceProvider) {
        throw std::invalid_argument("ServiceProvider cannot be null");
    }

    m_EventBus = m_ServiceProvider->Resolve<EventBus>();
    m_HookManager = m_ServiceProvider->Resolve<HookManager>();
    m_ScriptManager = m_ServiceProvider->Resolve<ScriptContextManager>();
    m_RecordPlayer = m_ServiceProvider->Resolve<RecordPlayer>();
    m_FrameTime = m_ServiceProvider->Resolve<ScriptFrameTime>();
    m_InputSystem = m_ServiceProvider->Resolve<InputSystem>();
    m_GameControl = m_ServiceProvider->Resolve<IGameControl>();
    m_InputAccess = m_ServiceProvider->Resolve<IInputAccess>();
#ifdef ENABLE_REPL
    m_REPLServer = m_ServiceProvider->Resolve<LuaREPLServer>();
#endif
}

PlaybackService::~PlaybackService() {
    if (m_IsPlaying) {
        StopPlaybackImmediate();
    }
}

Result<void> PlaybackService::PreparePlayback(TASProject *project, PlaybackType type) {
    if (m_IsPlaying) {
        return Result<void>::Error("Already playing", "playback_service");
    }
    if (m_IsPrepared) {
        return Result<void>::Error("Playback already prepared", "playback_service");
    }
    if (!project) {
        return Result<void>::Error("Project cannot be null", "playback_service");
    }
    if (type == PlaybackType::None) {
        return Result<void>::Error("Invalid playback type", "playback_service");
    }
    if (type == PlaybackType::Record) {
        // Startup autoplay can prepare while original resources are loading.
        // Readiness is required at activation, before record samples run.
        if (!project->IsRecordProject()) {
            return Result<void>::Error("Project is not a record", "playback_service");
        }
        if (!project->CanPlayRecord()) {
            std::string reason = project->GetValidationMessage().empty()
                                     ? "Record has no frames"
                                     : project->GetValidationMessage();
            return Result<void>::Error(reason, "playback_service");
        }
    }

    m_CurrentProject = project;
    m_Type = type;
    m_IsPrepared = true;
    m_CurrentTick = 0;
    m_CompletionSignaled = false;
    m_PlaybackContextName.clear();

    Log::Info("PlaybackService: %s playback prepared for '%s'.",
              type == PlaybackType::Script ? "Script" : "Record",
              project->GetName().c_str());
    return Result<void>::Ok();
}

Result<void> PlaybackService::ActivatePlayback() {
    if (m_IsPlaying) {
        return Result<void>::Error("Already playing", "playback_service");
    }
    if (!m_IsPrepared) {
        return Result<void>::Error("Playback is not prepared", "playback_service");
    }

    m_GameControl->AcquireKeyBindings();

    Result<void> result = (m_Type == PlaybackType::Script)
        ? ActivateScriptPlayback()
        : ActivateRecordPlayback();

    if (!result.IsOk()) {
        CleanupInputSystem();
        return result;
    }

    m_IsPrepared = false;
    m_IsPlaying = true;
    m_IsPaused = false;
    m_CurrentTick = 0;
    m_CompletionSignaled = false;
    // E487 diagnostic only: test an activation/input-phase discrepancy against
    // complete original traces. Skipping input zero is not a loading contract.
    const char *inputLead = std::getenv("BALLANCE_TAS_DIAG_INPUT_LEAD");
    if (m_Type == PlaybackType::Record && inputLead &&
        std::strcmp(inputLead, "1") == 0 && m_RecordPlayer->Seek(1)) {
        m_CurrentTick = 1;
        Log::Info("Diagnostic input origin: starting at record 1.");
    }
    if (m_Type == PlaybackType::Record)
        SetHostFastForward(true);

    Log::Info("PlaybackService: %s playback activated for '%s'.",
              m_Type == PlaybackType::Script ? "Script" : "Record",
              m_CurrentProject ? m_CurrentProject->GetName().c_str() : "?");
    return Result<void>::Ok();
}

Result<void> PlaybackService::StopPlaybackGraceful(bool clearProject) {
    SetHostFastForward(false);
    if (!m_IsPlaying && !m_IsPrepared) {
        return Result<void>::Error("Not playing", "playback_service");
    }

    if (m_IsPrepared && !m_IsPlaying) {
        m_IsPrepared = false;
        m_CompletionSignaled = false;
        if (clearProject) {
            m_CurrentProject = nullptr;
        }
        m_PlaybackContextName.clear();
        m_Type = PlaybackType::None;
        Log::Info("PlaybackService: Cancelled prepared playback.");
        return Result<void>::Ok();
    }

    RemoveHookCallbacks();
    if (m_FrameTime) {
        m_FrameTime->End();
    }

    if (m_Type == PlaybackType::Script) {
        auto ctx = m_ScriptManager->GetContext(m_PlaybackContextName);
        if (ctx && ctx->IsExecuting()) {
            ctx->Stop();
        }
    } else if (m_Type == PlaybackType::Record) {
        m_RecordPlayer->Stop();
    }

    CleanupInputSystem();

    auto *im = m_InputAccess->GetInputManager();
    if (im) {
        memset(im->GetKeyboardState(), KS_IDLE, 256);
    }

    m_IsPlaying = false;
    m_IsPaused = false;
    m_IsPrepared = false;
    m_CompletionSignaled = false;
    if (clearProject) {
        m_CurrentProject = nullptr;
    }
    m_PlaybackContextName.clear();
    m_Type = PlaybackType::None;

    Log::Info("PlaybackService: Stopped playback.");
    return Result<void>::Ok();
}

void PlaybackService::StopPlaybackImmediate() {
    SetHostFastForward(false);
    RemoveHookCallbacks();
    if (m_FrameTime) {
        m_FrameTime->End();
    }

    if (m_Type == PlaybackType::Script) {
        auto ctx = m_ScriptManager->GetContext(m_PlaybackContextName);
        if (ctx && ctx->IsExecuting()) {
            ctx->Stop();
        }
    } else if (m_Type == PlaybackType::Record) {
        m_RecordPlayer->Stop();
    }

    CleanupInputSystem();

    auto *im = m_InputAccess->GetInputManager();
    if (im) {
        memset(im->GetKeyboardState(), KS_IDLE, 256);
    }

    m_IsPlaying = false;
    m_IsPaused = false;
    m_IsPrepared = false;
    m_CompletionSignaled = false;
    m_CurrentProject = nullptr;
    m_PlaybackContextName.clear();
    m_Type = PlaybackType::None;
}

void PlaybackService::Pause() {
    if (!m_IsPlaying || m_IsPaused) {
        return;
    }

    if (m_Type == PlaybackType::Script) {
        auto ctx = m_ScriptManager->GetContext(m_PlaybackContextName);
        if (ctx && ctx->IsExecuting()) {
            ctx->Pause();
        }
    } else if (m_Type == PlaybackType::Record) {
        m_RecordPlayer->Pause();
    }

    m_IsPaused = true;
    Log::Info("PlaybackService: Paused.");
}

void PlaybackService::Resume() {
    if (!m_IsPlaying || !m_IsPaused) {
        return;
    }

    if (m_Type == PlaybackType::Script) {
        auto ctx = m_ScriptManager->GetContext(m_PlaybackContextName);
        if (ctx && ctx->IsExecuting()) {
            ctx->Resume();
        }
    } else if (m_Type == PlaybackType::Record) {
        m_RecordPlayer->Resume();
    }

    m_IsPaused = false;
    Log::Info("PlaybackService: Resumed.");
}

bool PlaybackService::RewindRecordTo(size_t tick) {
    if (!m_IsPlaying || m_Type != PlaybackType::Record || !m_RecordPlayer)
        return false;
    auto *input = m_InputAccess ? m_InputAccess->GetInputManager() : nullptr;
    if (!input || !input->GetKeyboardState())
        return false;
    if (!m_RecordPlayer->Seek(tick))
        return false;
    if (!m_RecordPlayer->RestoreConsumedFrameInput(input->GetKeyboardState()))
        return false;
    m_CurrentTick = tick;
    m_CompletionSignaled = false;
    return true;
}

float PlaybackService::GetProgress() const {
    if (!m_IsPlaying) {
        return 0.0f;
    }

    if (m_Type == PlaybackType::Record) {
        size_t total = m_RecordPlayer->GetTotalFrames();
        return total > 0 ? static_cast<float>(m_RecordPlayer->GetCurrentFrame()) / total : 0.0f;
    }

    return 0.0f;
}

Result<void> PlaybackService::ActivateScriptPlayback() {
    if (!m_CurrentProject) {
        return Result<void>::Error("No project set", "playback_service");
    }

    const bool isGlobal = m_CurrentProject->IsGlobalProject();
    auto *game = m_ServiceProvider->Resolve<GameInterface>();
    auto ctx = isGlobal
                   ? m_ScriptManager->GetOrCreateGlobalContext()
                   : m_ScriptManager->GetOrCreateLevelContext(ResolvePlaybackLevelName(m_CurrentProject, game));

    if (!ctx) {
        return Result<void>::Error("Failed to create script context", "playback_service");
    }
    // Before the script loads, so even its top level can change the frame time.
    if (m_FrameTime) {
        m_FrameTime->Begin(m_CurrentProject->GetInitialFrameTime());
    }

    if (!ctx->LoadAndExecute(m_CurrentProject)) {
        if (m_FrameTime) {
            m_FrameTime->End();
        }
        return Result<void>::Error("Failed to load and execute script", "playback_service");
    }

    m_PlaybackContextName = ctx->GetName();
    m_InputSystem->SetEnabled(true);
    m_InputSystem->Reset();

    InstallScriptCallbacks();
    return Result<void>::Ok();
}

Result<void> PlaybackService::ActivateRecordPlayback() {
    const auto preloadError = m_GameControl->GetLegacyPreloadError();
    if (!preloadError.empty()) return Result<void>::Error(preloadError, "playback_service");
    if (!m_CurrentProject) {
        return Result<void>::Error("No project set", "playback_service");
    }

    if (!m_CurrentProject->CanPlayRecord()) {
        std::string reason = m_CurrentProject->GetValidationMessage().empty()
                                 ? "Record has no frames"
                                 : m_CurrentProject->GetValidationMessage();
        return Result<void>::Error(reason, "playback_service");
    }

    if (!m_RecordPlayer->LoadAndPlay(m_CurrentProject)) {
        std::string reason = m_CurrentProject->GetValidationMessage().empty()
                                 ? "Failed to load record for playback"
                                 : m_CurrentProject->GetValidationMessage();
        return Result<void>::Error(reason, "playback_service");
    }

    m_InputSystem->SetEnabled(false);
    m_InputSystem->Reset();

    InstallRecordCallbacks();
    return Result<void>::Ok();
}

void PlaybackService::InstallScriptCallbacks() {
    m_PostTickGuard = m_HookManager->RegisterPostTickCallback(
        [this](CKTimeManager *tm) {
            if (!m_IsPlaying || m_IsPaused) {
                return;
            }

            if (m_FrameTime && m_FrameTime->IsActive()) {
                tm->SetLastDeltaTime(m_FrameTime->Get());
            } else if (m_CurrentProject && m_CurrentProject->IsValid()) {
                tm->SetLastDeltaTime(m_CurrentProject->GetInitialFrameTime());
            }
        });

    m_PostInputGuard = m_HookManager->RegisterPostInputCallback(
        [this](CKInputManager *im) {
            if (!m_IsPlaying || m_IsPaused) {
                return;
            }

#ifdef ENABLE_REPL
            if (m_REPLServer && m_REPLServer->IsRunning()) {
                m_REPLServer->OnTickStart(m_CurrentTick);
                m_REPLServer->ProcessImmediateCommands();
            }
#endif

            m_ScriptManager->TickAll();
            // A script stopped the playback during this tick: that is not a completion.
            if (!m_IsPlaying) {
                return;
            }

            ApplyMergedContextInputs(im);

            ++m_CurrentTick;

            auto playbackContext = m_ScriptManager->GetContext(m_PlaybackContextName);
            const bool playbackRunning = playbackContext && playbackContext->IsExecuting();
            if (!m_CompletionSignaled && !playbackRunning) {
                m_CompletionSignaled = true;
                m_EventBus->Publish(PlaybackCompletedEvent{static_cast<int>(PlaybackType::Script)});
                if (m_CompletionCallback) {
                    m_CompletionCallback();
                }
                if (m_IsPlaying && m_Type == PlaybackType::Script) {
                    StopPlaybackGraceful(false);
                }
            }

#ifdef ENABLE_REPL
            if (m_REPLServer && m_REPLServer->IsRunning()) {
                m_REPLServer->OnTickEnd(m_CurrentTick);
            }
#endif
        });
}

void PlaybackService::InstallRecordCallbacks() {
    m_PostTickGuard = m_HookManager->RegisterPostTickCallback(
        [this](CKTimeManager *tm) {
            if (!m_IsPlaying || m_IsPaused) {
                return;
            }
            if (m_RecordPlayer->IsPlaying()) {
                TraceRecordPhase("post-time-pre", m_CurrentTick, m_RecordPlayer,
                                 m_ServiceProvider->Resolve<GameInterface>(), nullptr);
                tm->SetLastDeltaTime(m_RecordPlayer->GetFrameDeltaTime(m_CurrentTick));
                TraceRecordPhase("post-time-post", m_CurrentTick, m_RecordPlayer,
                                 m_ServiceProvider->Resolve<GameInterface>(), nullptr);
            }
        });

    m_PostInputGuard = m_HookManager->RegisterPostInputCallback(
        [this](CKInputManager *im) {
            if (!m_IsPlaying || m_IsPaused) {
                return;
            }

            auto *game = m_ServiceProvider->Resolve<GameInterface>();
            TraceRecordPhase("post-input-enter", m_CurrentTick, m_RecordPlayer, game, im);

            // A record owns the input sequence from level-load activation onward.
            // Consume each sample once, including while the ball is not physical.
            // Waiting for a particular ball state and then seeking over samples
            // changes the recording's clock/input contract.
            if (m_RecordPlayer->IsPlaying()) {
                m_RecordPlayer->Tick(m_CurrentTick, im->GetKeyboardState());
                TraceRecordPhase("post-input-applied", m_CurrentTick, m_RecordPlayer, game, im);
                ++m_CurrentTick;
            }

            if (!m_CompletionSignaled && !m_RecordPlayer->IsPlaying()) {
                m_CompletionSignaled = true;
                Log::Info("PlaybackService: Record playback completed naturally.");
                m_EventBus->Publish(PlaybackCompletedEvent{static_cast<int>(PlaybackType::Record)});
                if (m_CompletionCallback) {
                    m_CompletionCallback();
                }
                if (m_IsPlaying && m_Type == PlaybackType::Record) {
                    StopPlaybackGraceful(false);
                }
            }
        });
}

void PlaybackService::RemoveHookCallbacks() {
    m_PostTickGuard.Reset();
    m_PostInputGuard.Reset();
}

void PlaybackService::ApplyMergedContextInputs(CKInputManager *inputManager) {
    if (!inputManager) {
        return;
    }

    auto contexts = m_ScriptManager->GetContextsByPriority();
    if (contexts.empty()) {
        return;
    }

    std::vector<InputSystem *> activeInputs;
    for (const auto &ctx : contexts) {
        if (ctx && ctx->IsExecuting()) {
            InputSystem *inputSys = const_cast<InputSystem *>(ctx->GetInputSystem());
            if (inputSys && inputSys->IsEnabled()) {
                activeInputs.push_back(inputSys);
            }
        }
    }
    if (activeInputs.empty()) {
        return;
    }

    InputSystem::ApplyMergedToKeyboardState(m_CurrentTick, activeInputs, inputManager->GetKeyboardState());
}

void PlaybackService::SetupInputSystem() {
    m_InputSystem->Reset();
}

void PlaybackService::CleanupInputSystem() {
    m_InputSystem->Reset();
    m_InputSystem->SetEnabled(false);
}
