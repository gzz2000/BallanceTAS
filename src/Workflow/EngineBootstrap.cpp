#include "EngineBootstrap.h"

#include <filesystem>

#include "TASConstants.h"
#include "Runtime/ContextLifecycleCoordinator.h"
#include "EventBus.h"
#include "EventManager.h"
#include "GameInterface.h"
#include "HookManager.h"
#include "HarnessRuntime.h"
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
#include "ScriptContextManager.h"
#include "ScriptGenerator.h"
#include "ServiceContainer.h"
#include "StartupProjectManager.h"
#include "TASEngine.h"
#include "TASProject.h"
#include "TranslationService.h"
#include "UIManager.h"
#include "ValidationService.h"
#include "ScriptFrameTime.h"
#include "HudWatches.h"

#ifdef ENABLE_REPL
#include "LuaREPLServer.h"
#endif

namespace {

RuntimeSession::Hooks BuildRuntimeSessionHooks(TASEngine &engine) {
    RuntimeSession::Hooks hooks;
    auto *services = &engine.GetServiceProvider();

    hooks.prepareRecording = [services](RuntimeSession::RecordingOptions options) {
        auto *service = services->Resolve<RecordingService>();
        return service
            ? service->PrepareRecording(options.validation)
            : Result<void>::Error("RecordingService not available", "runtime_session");
    };
    hooks.activateRecording = [services]() {
        auto *service = services->Resolve<RecordingService>();
        return service
            ? service->ActivateRecording()
            : Result<void>::Error("RecordingService not available", "runtime_session");
    };
    hooks.stopRecordingGraceful = [services]() {
        auto *service = services->Resolve<RecordingService>();
        if (!service) {
            return Result<void>::Ok();
        }
        auto result = service->StopRecordingGraceful();
        return result.IsOk() ? Result<void>::Ok() : Result<void>::Error(result.GetError());
    };
    hooks.stopRecordingImmediate = [services]() {
        if (auto *service = services->Resolve<RecordingService>()) {
            service->StopRecordingImmediate();
        }
    };
    hooks.isRecordingPrepared = [services]() {
        auto *service = services->Resolve<RecordingService>();
        return service && service->IsPrepared();
    };
    hooks.isRecordingActive = [services]() {
        auto *service = services->Resolve<RecordingService>();
        return service && service->IsRecording();
    };

    hooks.preparePlayback = [services](TASProject *project, PlaybackType type) {
        auto *service = services->Resolve<PlaybackService>();
        return service
            ? service->PreparePlayback(project, type)
            : Result<void>::Error("PlaybackService not available", "runtime_session");
    };
    hooks.activatePlayback = [services]() {
        auto *service = services->Resolve<PlaybackService>();
        return service
            ? service->ActivatePlayback()
            : Result<void>::Error("PlaybackService not available", "runtime_session");
    };
    hooks.stopPlaybackGraceful = [services](bool clearProject) {
        auto *service = services->Resolve<PlaybackService>();
        return service ? service->StopPlaybackGraceful(clearProject) : Result<void>::Ok();
    };
    hooks.stopPlaybackImmediate = [services]() {
        if (auto *service = services->Resolve<PlaybackService>()) {
            service->StopPlaybackImmediate();
        }
    };
    hooks.pausePlayback = [services]() {
        if (auto *service = services->Resolve<PlaybackService>()) {
            service->Pause();
        }
    };
    hooks.resumePlayback = [services]() {
        if (auto *service = services->Resolve<PlaybackService>()) {
            service->Resume();
        }
    };
    hooks.isPlaybackPrepared = [services]() {
        auto *service = services->Resolve<PlaybackService>();
        return service && service->IsPrepared();
    };
    hooks.isPlaybackActiveOrPaused = [services]() {
        auto *service = services->Resolve<PlaybackService>();
        return service && (service->IsPlaying() || service->IsPaused());
    };
    hooks.currentPlaybackProject = [services]() {
        auto *service = services->Resolve<PlaybackService>();
        return service ? service->GetCurrentProject() : nullptr;
    };

    hooks.prepareTranslation = [services](TASProject *project) {
        auto *service = services->Resolve<TranslationService>();
        return service
            ? service->PrepareTranslation(project)
            : Result<void>::Error("TranslationService not available", "runtime_session");
    };
    hooks.activateTranslation = [services]() {
        auto *service = services->Resolve<TranslationService>();
        return service
            ? service->ActivateTranslation()
            : Result<void>::Error("TranslationService not available", "runtime_session");
    };
    hooks.stopTranslationGraceful = [services](bool clearProject) {
        auto *service = services->Resolve<TranslationService>();
        return service ? service->StopTranslationGraceful(clearProject) : Result<void>::Ok();
    };
    hooks.stopTranslationImmediate = [services]() {
        if (auto *service = services->Resolve<TranslationService>()) {
            service->StopTranslationImmediate();
        }
    };
    hooks.isTranslationPrepared = [services]() {
        auto *service = services->Resolve<TranslationService>();
        return service && service->IsPrepared();
    };
    hooks.isTranslationActive = [services]() {
        auto *service = services->Resolve<TranslationService>();
        return service && service->IsTranslating();
    };

    hooks.startValidationForPlayback = [services, &engine](TASProject *project) {
        auto *validation = services->Resolve<ValidationService>();
        auto *playback = services->Resolve<PlaybackService>();
        if (!validation || !playback || validation->IsActive()) {
            return Result<void>::Ok();
        }
        std::string outputPath;
        if (project) {
            std::filesystem::path path(project->GetPath());
            if (path.has_extension()) {
                path = path.parent_path();
            }
            outputPath = path.string();
            if (!outputPath.empty()) outputPath.push_back(std::filesystem::path::preferred_separator);
        }
        if (outputPath.empty()) {
            return Result<void>::Ok();
        }
        ValidationConfig config;
        config.exact = engine.IsRequestedValidationExact();
        validation->SetConfig(config);
        return validation->Start(outputPath, engine.GetRequestedValidationReference());
    };
    hooks.stopValidationGraceful = [services]() {
        auto *validation = services->Resolve<ValidationService>();
        return validation && validation->IsActive() ? validation->Stop() : Result<void>::Ok();
    };
    hooks.stopValidationImmediate = [services]() {
        auto *validation = services->Resolve<ValidationService>();
        if (validation && validation->IsActive()) {
            validation->StopImmediate();
        }
    };
    hooks.isValidationActive = [services]() {
        auto *validation = services->Resolve<ValidationService>();
        return validation && validation->IsActive();
    };

    hooks.onStateEntered = [&engine, services](RuntimeSession::State state, RuntimeSession::State) {
        auto *game = services->Resolve<GameInterface>();
        auto *input = services->Resolve<InputSystem>();
        const auto resetInput = [input]() {
            if (input) {
                input->Reset();
                input->SetEnabled(false);
            }
        };

        switch (state) {
        case RuntimeSession::State::Idle:
            resetInput();
            if (game) {
                game->SetUIMode(UIMode::Idle);
            }
            engine.ClearControlRequests();
            break;
        case RuntimeSession::State::PendingRecord:
        case RuntimeSession::State::PendingScriptPlayback:
        case RuntimeSession::State::PendingRecordPlayback:
        case RuntimeSession::State::PendingTranslation:
            if (game) {
                game->SetUIMode(UIMode::Idle);
            }
            break;
        case RuntimeSession::State::Recording:
        case RuntimeSession::State::Translating:
            if (game) {
                game->SetUIMode(UIMode::Recording);
            }
            break;
        case RuntimeSession::State::PlayingScript:
        case RuntimeSession::State::PlayingRecord:
            if (game) {
                game->SetUIMode(UIMode::Playing);
            }
            break;
        case RuntimeSession::State::Paused:
            if (game) {
                game->SetUIMode(UIMode::Paused);
            }
            break;
        case RuntimeSession::State::ShuttingDown:
            resetInput();
            if (game) {
                game->SetUIMode(UIMode::Idle);
            }
            engine.ClearControlRequests();
            break;
        }
    };

    return hooks;
}

} // namespace

bool EngineBootstrap::InitializeCoreSubsystems(TASEngine &engine) {
    try {
        auto &c = engine.m_ServiceContainer;

        // Register externally-owned pointers (BallanceTAS lifetime)
        c.RegisterSingletonPtr<GameInterface>(engine.m_GameInterface);
        c.RegisterSingletonPtr<EventBus>(engine.m_EventBus);
        c.RegisterSingletonPtr<HookManager>(engine.m_HookManager);

        // Register GameInterface against narrow ISP interfaces
        c.RegisterSingletonPtr<IGameControl>(engine.m_GameInterface);
        c.RegisterSingletonPtr<IGameQuery>(engine.m_GameInterface);
        c.RegisterSingletonPtr<IInputAccess>(engine.m_GameInterface);
        c.RegisterSingletonPtr<IPhysicsProvider>(engine.m_GameInterface);
        c.RegisterSingletonPtr<IObjectProvider>(engine.m_GameInterface);
        c.RegisterSingletonPtr<IGameStateProvider>(engine.m_GameInterface);
        c.RegisterSingletonPtr<ITimeProvider>(engine.m_GameInterface);

        // Register OperationRequestStore (non-owning ptr to TASEngine's member)
        c.RegisterSingletonPtr<OperationRequestStore>(&engine.m_Requests);

        // Core subsystems — container takes ownership
        c.RegisterSingletonInstance<InputSystem>(std::make_unique<InputSystem>());
        c.RegisterSingletonInstance<ScriptFrameTime>(std::make_unique<ScriptFrameTime>());
        c.RegisterSingletonInstance<HudWatches>(std::make_unique<HudWatches>());
        c.RegisterSingletonInstance<EventManager>(std::make_unique<EventManager>());
        c.RegisterSingletonInstance<Recorder>(std::make_unique<Recorder>(&engine));
        c.RegisterSingletonInstance<ScriptGenerator>(std::make_unique<ScriptGenerator>(&engine));
        c.RegisterSingletonInstance<ScriptContextManager>(std::make_unique<ScriptContextManager>(&engine));
        c.RegisterSingletonInstance<RecordPlayer>(std::make_unique<RecordPlayer>(&engine));
        c.RegisterSingletonInstance<StartupProjectManager>(std::make_unique<StartupProjectManager>(&engine));
        c.RegisterSingletonInstance<ProjectManager>(std::make_unique<ProjectManager>(&engine));
        c.RegisterSingletonInstance<HarnessRuntime>(std::make_unique<HarnessRuntime>(&engine));

        // Services with dependencies
        c.RegisterSingletonInstance<RecordingService>(
            std::make_unique<RecordingService>(&engine.m_ServiceProvider));
        c.RegisterSingletonInstance<TranslationService>(
            std::make_unique<TranslationService>(
                *c.Resolve<Recorder>(),
                *c.Resolve<RecordPlayer>(),
                *engine.m_GameInterface,
                *engine.m_GameInterface,
                *engine.m_GameInterface,
                *c.Resolve<ScriptContextManager>(),
                *c.Resolve<InputSystem>(),
                *engine.m_HookManager,
                *engine.m_EventBus,
                *c.Resolve<ScriptFrameTime>(),
                engine.GetPath()));
        c.RegisterSingletonInstance<ValidationService>(
            std::make_unique<ValidationService>(
                *c.Resolve<Recorder>(),
                *engine.m_EventBus,
                *engine.m_HookManager));
        c.RegisterSingletonInstance<SavestateManager>(
            std::make_unique<SavestateManager>(engine.m_ServiceProvider));

        Log::Info("Core runtime subsystems initialized.");
        return true;
    } catch (const std::exception &e) {
        Log::Error("Failed to initialize core subsystems: %s", e.what());
        return false;
    }
}

bool EngineBootstrap::InitializeHighLevelSubsystems(TASEngine &engine) {
    try {
        auto &c = engine.m_ServiceContainer;

        auto *scriptContextManager = c.Resolve<ScriptContextManager>();
        if (scriptContextManager && !scriptContextManager->Initialize()) {
            Log::Error("Failed to initialize ScriptContextManager.");
            return false;
        }

        LuaREPLServer *replPtr = nullptr;
#ifdef ENABLE_REPL
        c.RegisterSingletonInstance<LuaREPLServer>(
            std::make_unique<LuaREPLServer>(&engine));
        auto *replServer = c.Resolve<LuaREPLServer>();
        if (replServer->Initialize(TASConstants::DefaultREPLPort, "")) {
            if (replServer->Start()) {
                Log::Info("REPL server started on port %u", TASConstants::DefaultREPLPort);
                replPtr = replServer;
            } else {
                Log::Warn("Failed to start REPL server.");
            }
        } else {
            Log::Warn("Failed to initialize REPL server.");
        }
#endif
        c.RegisterSingletonInstance<PlaybackService>(
            std::make_unique<PlaybackService>(&engine.m_ServiceProvider));
        if (auto *playbackService = c.Resolve<PlaybackService>()) {
            playbackService->SetCompletionCallback([&engine]() {
                engine.HandlePlaybackCompleted();
            });
        }
        c.RegisterSingletonInstance<RuntimeSession>(
            std::make_unique<RuntimeSession>(engine.m_EventBus, BuildRuntimeSessionHooks(engine)));

        auto *startupProjectManager = c.Resolve<StartupProjectManager>();
        if (startupProjectManager && !startupProjectManager->Initialize()) {
            Log::Error("Failed to initialize StartupProjectManager.");
            return false;
        }

        c.RegisterSingletonInstance<ContextLifecycleCoordinator>(
            std::make_unique<ContextLifecycleCoordinator>(
                *scriptContextManager,
                *engine.m_GameInterface));
        c.RegisterSingletonInstance<LuaTypedEventBridge>(
            std::make_unique<LuaTypedEventBridge>(
                *engine.m_EventBus,
                *scriptContextManager,
                c.Resolve<Recorder>(),
                *c.Resolve<RuntimeSession>(),
                [&engine]() { return engine.GetCurrentTick(); }));
        c.RegisterSingletonInstance<RuntimeEventRouter>(
            std::make_unique<RuntimeEventRouter>(
                *engine.m_EventBus,
                *c.Resolve<RuntimeSession>(),
                *c.Resolve<ContextLifecycleCoordinator>(),
                engine.m_Requests,
                [&engine]() { return engine.LevelLoadRunsTAS(); }));

        auto *bridge = c.Resolve<LuaTypedEventBridge>();
        if (bridge) {
            bridge->Initialize();
        }
        auto *router = c.Resolve<RuntimeEventRouter>();
        if (router) {
            router->Initialize();
        }

        Log::Info("TASEngine and all subsystems initialized.");
        return true;
    } catch (const std::exception &e) {
        Log::Error("Failed to initialize higher-level subsystems: %s", e.what());
        return false;
    }
}
