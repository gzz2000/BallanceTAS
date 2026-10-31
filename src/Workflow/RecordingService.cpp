/**
 * @file RecordingService.cpp
 * @brief Implementation of RecordingService - full recording lifecycle.
 */

#include "RecordingService.h"

#include "ServiceContainer.h"
#include "GameInterface.h"
#include "IGameControl.h"
#include "Recorder.h"
#include "InputSystem.h"
#include "Logger.h"

#include <CKInputManager.h>
#include <CKTimeManager.h>

RecordingService::RecordingService(ServiceProvider *provider)
    : m_ServiceProvider(provider) {
    if (!m_ServiceProvider) {
        throw std::invalid_argument("ServiceProvider cannot be null");
    }

    m_EventBus = m_ServiceProvider->Resolve<EventBus>();
    m_HookManager = m_ServiceProvider->Resolve<HookManager>();
    m_Recorder = m_ServiceProvider->Resolve<Recorder>();
    m_InputSystem = m_ServiceProvider->Resolve<InputSystem>();
    m_GameControl = m_ServiceProvider->Resolve<IGameControl>();
    m_GameInterface = m_ServiceProvider->Resolve<GameInterface>();
}

RecordingService::~RecordingService() {
    if (m_IsRecording) {
        StopRecordingImmediate();
    }
}

Result<void> RecordingService::PrepareRecording(bool useValidation) {
    // Preparing a request does not sample input or advance record physics.
    // ActivateRecording checks initialization before installing any callbacks.
    if (m_IsRecording) {
        return Result<void>::Error("Already recording", "recording_service");
    }
    if (m_IsPrepared) {
        return Result<void>::Error("Recording already prepared", "recording_service");
    }
    m_UseValidation = useValidation;
    m_IsPrepared = true;
    m_CurrentTick = 0;

    Log::Info("RecordingService: Recording prepared.");
    return Result<void>::Ok();
}

Result<void> RecordingService::ActivateRecording() {
    const auto preloadError = m_GameControl->GetLegacyPreloadError();
    if (!preloadError.empty()) return Result<void>::Error(preloadError, "recording_service");
    if (m_IsRecording) {
        return Result<void>::Error("Already recording", "recording_service");
    }
    if (!m_IsPrepared) {
        return Result<void>::Error("Recording is not prepared", "recording_service");
    }
    m_GameControl->AcquireKeyBindings();

    SetupInputSystem();
    m_Recorder->Start();
    InstallHookCallbacks();

    m_IsPrepared = false;
    m_IsRecording = true;
    m_CurrentTick = 0;

    Log::Info("RecordingService: Recording activated%s.",
              m_UseValidation ? " (validation)" : "");
    return Result<void>::Ok();
}

Result<RecordingResult> RecordingService::StopRecordingGraceful() {
    if (!m_IsRecording && !m_IsPrepared) {
        return Result<RecordingResult>::Error("Not recording", "recording_service");
    }

    if (m_IsPrepared && !m_IsRecording) {
        m_IsPrepared = false;
        m_UseValidation = false;
        Log::Info("RecordingService: Cancelled prepared recording.");
        return Result<RecordingResult>::Ok(RecordingResult{{}, 0});
    }

    RemoveHookCallbacks();

    std::vector<FrameData> frames = m_Recorder->Stop();

    CleanupInputSystem();

    m_IsRecording = false;
    m_IsPrepared = false;
    m_UseValidation = false;

    RecordingResult result;
    result.frames = std::move(frames);
    result.totalFrames = result.frames.size();

    Log::Info("RecordingService: Stopped recording - captured %zu frames.",
              result.totalFrames);
    return Result<RecordingResult>::Ok(std::move(result));
}

void RecordingService::StopRecordingImmediate() {
    RemoveHookCallbacks();

    if (m_Recorder->IsRecording()) {
        m_Recorder->SetAutoGenerate(false);
        m_Recorder->Stop();
    }

    CleanupInputSystem();

    m_IsRecording = false;
    m_IsPrepared = false;
    m_UseValidation = false;
}

size_t RecordingService::GetFrameCount() const {
    return m_Recorder->GetTotalFrames();
}

void RecordingService::InstallHookCallbacks() {
    m_PostTickGuard = m_HookManager->RegisterPostTickCallback(
        [this](CKTimeManager *tm) {
            if (!m_IsRecording) {
                return;
            }
            tm->SetLastDeltaTime(m_Recorder->GetDeltaTime());
        });

    m_PostInputGuard = m_HookManager->RegisterPostInputCallback(
        [this](CKInputManager *im) {
            if (!m_IsRecording) {
                return;
            }
            m_Recorder->Tick(m_CurrentTick, im->GetKeyboardState());
            ++m_CurrentTick;
        });
}

void RecordingService::RemoveHookCallbacks() {
    m_PostTickGuard.Reset();
    m_PostInputGuard.Reset();
}

void RecordingService::SetupInputSystem() {
    m_InputSystem->Reset();
    m_InputSystem->SetEnabled(false);
}

void RecordingService::CleanupInputSystem() {
    m_InputSystem->Reset();
    m_InputSystem->SetEnabled(false);
}
