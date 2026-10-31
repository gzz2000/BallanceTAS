#include "PlayerHostBridge.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstring>
#include <filesystem>

#include "GameEvents.h"
#include "Logger.h"
#include "PlaybackService.h"
#include "ProjectManager.h"
#include "RecordPlayer.h"
#include "RuntimeSession.h"
#include "ServiceContainer.h"
#include "TASEngine.h"
#include "TASProject.h"

namespace {
struct HostDiscovery {
    const BallanceTasHostApi *api = nullptr;
    BallanceTasHostLaunchSpec launch{};
};

bool Discover(HostDiscovery &result) {
#ifdef _WIN32
    HMODULE executable = ::GetModuleHandleW(nullptr);
    if (!executable) return false;

    auto getApi = reinterpret_cast<BallanceTasHostGetApiFn>(
        ::GetProcAddress(executable, "BallanceTasHost_GetApi"));
#else
    auto getApi = reinterpret_cast<BallanceTasHostGetApiFn>(
        dlsym(RTLD_DEFAULT, "BallanceTasHost_GetApi"));
#endif
    if (!getApi) return false;

    const BallanceTasHostApi *api = getApi(BALLANCE_TAS_HOST_ABI_VERSION);
    if (!api || api->abi_version != BALLANCE_TAS_HOST_ABI_VERSION ||
        api->size < sizeof(BallanceTasHostApi) || !api->get_launch_spec ||
        !api->register_runtime || !api->unregister_runtime) {
        Log::Error("TAS Player host ABI is incompatible (required version %u).",
                   BALLANCE_TAS_HOST_ABI_VERSION);
        return false;
    }

    BallanceTasHostLaunchSpec launch{};
    launch.size = sizeof(launch);
    launch.abi_version = BALLANCE_TAS_HOST_ABI_VERSION;
    if (!api->get_launch_spec(&launch) || !launch.target_utf8 || !*launch.target_utf8) {
        return false;
    }

    result.api = api;
    result.launch = launch;
    return true;
}
}

PlayerHostBridge::PlayerHostBridge(TASEngine *engine) : m_Engine(engine) {}

PlayerHostBridge::~PlayerHostBridge() {
    Shutdown();
}

bool PlayerHostBridge::IsRequested() {
    HostDiscovery discovery;
    return Discover(discovery);
}

bool PlayerHostBridge::DiscoverHost() {
    HostDiscovery discovery;
    if (!Discover(discovery)) return false;
    m_HostApi = discovery.api;
    m_LaunchSpec = discovery.launch;
    m_Target = discovery.launch.target_utf8;
    m_LaunchSpec.target_utf8 = m_Target.c_str();
    return true;
}

bool PlayerHostBridge::Initialize() {
    if (m_Registered) return true;
    if (!m_Engine || !DiscoverHost()) return false;

    m_Callbacks.size = sizeof(m_Callbacks);
    m_Callbacks.abi_version = BALLANCE_TAS_HOST_ABI_VERSION;
    m_Callbacks.query_status = &PlayerHostBridge::QueryStatusThunk;
    if (!m_HostApi->register_runtime(&m_Callbacks, this)) {
        Log::Error("TAS Player rejected BallanceTAS runtime registration.");
        return false;
    }
    m_Registered = true;

    auto &services = m_Engine->GetServiceProvider();
    auto *projects = services.Resolve<ProjectManager>();
    m_Playback = services.Resolve<PlaybackService>();
    m_RecordPlayer = services.Resolve<RecordPlayer>();
    m_Session = services.Resolve<RuntimeSession>();
    auto *eventBus = services.Resolve<EventBus>();
    if (!projects || !m_Playback || !m_RecordPlayer || !m_Session || !eventBus) {
        Fail("BallanceTAS runtime services are incomplete");
        return true;
    }

    m_CompletionSubscription = eventBus->Subscribe<PlaybackCompletedEvent>(
        [this](const PlaybackCompletedEvent &event) { OnPlaybackCompleted(event.playbackType); });
    // Published before StopOnFinish stops playback, so a finished run is not mistaken for an abort.
    m_LevelFinishSubscription = eventBus->Subscribe<LevelFinishEvent>(
        [this](const LevelFinishEvent &) { OnLevelFinish(); });

    std::string error;
    m_Project = projects->ResolveLaunchTarget(m_Target, error);
    if (!m_Project) {
        Fail(error.empty() ? "TAS project target could not be resolved" : error);
        return true;
    }

    m_ProjectName = m_Project->GetName();
    m_ProjectType = m_Project->IsRecordProject()
                        ? BALLANCE_TAS_PROJECT_RECORD
                        : BALLANCE_TAS_PROJECT_LUA;
    projects->SetCurrentProject(m_Project);

    // Arm the same way the TAS menu does: the session stays pending and
    // RuntimeEventRouter activates it at level load, so host and menu runs
    // start on the same frame.
    if (!m_Engine->StartReplay()) {
        Fail("TAS project could not be prepared for playback");
        return true;
    }
    m_PlaybackArmed = true;

    if (m_Project->IsScriptProject() && m_Project->IsGlobalProject() &&
        m_Project->ShouldExecuteOnStartup()) {
        // Startup projects run immediately instead of waiting for a level.
        auto activation = m_Session->OnLevelStart();
        if (!activation.IsOk()) {
            Fail("Global TAS project activation failed: " + activation.GetError().message);
            return true;
        }
        m_Message = "Ready before first input frame";
    } else {
        m_Message = "Waiting for level entry";
    }

    Log::Info("TAS Player target prepared: %s", m_Target.c_str());
    return true;
}

void PlayerHostBridge::Shutdown() {
    m_CompletionSubscription.Unsubscribe();
    m_LevelFinishSubscription.Unsubscribe();
    if (m_Registered && m_HostApi && m_HostApi->unregister_runtime) {
        m_HostApi->unregister_runtime(this);
    }
    m_Registered = false;
    m_HostApi = nullptr;
    m_Playback = nullptr;
    m_RecordPlayer = nullptr;
    m_Session = nullptr;
    m_Project = nullptr;
}

int BALLANCE_TAS_HOST_CALL PlayerHostBridge::QueryStatusThunk(
    void *runtime, BallanceTasRuntimeStatus *status) {
    return runtime ? static_cast<PlayerHostBridge *>(runtime)->QueryStatus(status) : 0;
}

int PlayerHostBridge::QueryStatus(BallanceTasRuntimeStatus *status) {
    if (!status || status->abi_version != BALLANCE_TAS_HOST_ABI_VERSION ||
        status->size < sizeof(BallanceTasRuntimeStatus)) {
        return 0;
    }

    const size_t consumed = m_Playback ? m_Playback->GetCurrentTick() : 0;
    status->size = sizeof(*status);
    status->abi_version = BALLANCE_TAS_HOST_ABI_VERSION;
    status->project_type = m_ProjectType;
    status->project_name_utf8 = m_ProjectName.c_str();
    status->consumed_frames = m_Completed ? m_CompletedFrames : static_cast<uint64_t>(consumed);
    status->total_frames = m_Project && m_Project->IsRecordProject()
                               ? static_cast<uint64_t>(m_Project->GetRecordFrameCount())
                               : BALLANCE_TAS_HOST_UNKNOWN_FRAME_COUNT;
    status->next_delta_ms = 0.0f;
    status->message_utf8 = m_Message.c_str();

    if (m_Failed) {
        status->phase = BALLANCE_TAS_RUNTIME_FAILED;
    } else if (m_Completed) {
        status->phase = BALLANCE_TAS_RUNTIME_COMPLETED;
    } else if (m_Playback && m_Playback->IsPlaying()) {
        status->phase = consumed == 0 ? BALLANCE_TAS_RUNTIME_ARMED : BALLANCE_TAS_RUNTIME_PLAYING;
        if (m_Project && m_Project->IsRecordProject() && m_RecordPlayer) {
            status->next_delta_ms = m_RecordPlayer->GetFrameDeltaTimeByFrame(consumed);
        } else if (m_Project) {
            status->next_delta_ms = m_Project->GetDeltaTime();
        }
        m_Message = consumed == 0 ? "Ready before first input frame" : "Playback active";
        status->message_utf8 = m_Message.c_str();
    } else {
        status->phase = BALLANCE_TAS_RUNTIME_WAITING;
    }
    return 1;
}

void PlayerHostBridge::Fail(std::string message) {
    m_Failed = true;
    m_Message = std::move(message);
    Log::Error("TAS Player host failure: %s", m_Message.c_str());
}

void PlayerHostBridge::OnPlaybackCompleted(int playbackType) {
    if (m_Completed || m_Failed || !m_Playback) return;
    const bool isRecord = playbackType == static_cast<int>(PlaybackType::Record);
    if ((isRecord && m_ProjectType != BALLANCE_TAS_PROJECT_RECORD) ||
        (!isRecord && m_ProjectType != BALLANCE_TAS_PROJECT_LUA)) {
        return;
    }
    m_CompletedFrames = static_cast<uint64_t>(m_Playback->GetCurrentTick());
    m_Completed = true;
    m_Message = "Playback completed";
}

void PlayerHostBridge::OnLevelFinish() {
    if (!m_PlaybackArmed || m_Completed || m_Failed || !m_Playback || !m_Playback->IsPlaying()) return;
    m_CompletedFrames = static_cast<uint64_t>(m_Playback->GetCurrentTick());
    m_Completed = true;
    m_Message = "Level finished";
}

void PlayerHostBridge::Tick() {
    if (!m_PlaybackArmed || m_Completed || m_Failed || !m_Session) return;
    // Natural completion sets m_Completed synchronously before the session
    // returns to Idle, so reaching Idle here means the run was stopped early
    // (stop hotkey, level exit, engine stop).
    if (m_Session->IsIdle()) {
        Fail("Playback stopped before completion");
    }
}
