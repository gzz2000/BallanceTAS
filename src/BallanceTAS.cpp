#include "BallanceTAS.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

#ifdef TAS_SOURCE_ENGINE
#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif
#include <PhysicsRuntimeApi.h>
#endif

#include <BML/BML.h>
#include <BML/Bui.h>
#include <BML/ScriptHelper.h>

#include "physics_RT.h"
#include "TASHook.h"
#include "PhysicsDeterminism.h"
#include "TASEngine.h"
#include "InGameOSD.h"
#include "Recorder.h"
#include "GameInterface.h"
#include "ServiceContainer.h"
#include "ScriptContextManager.h"
#include "StartupProjectManager.h"
#include "ProjectManager.h"
#include "PlaybackService.h"
#include "TASProject.h"
#include "UIManager.h"
#include "Logger.h"
#include "BMLLogSink.h"
#include "EventBus.h"
#include "GameEvents.h"
#include "HookManager.h"
#include "InputSystem.h"
#include "ScriptContext.h"
#include "HarnessRuntime.h"
#include "PlayerHostBridge.h"
#include "Diagnostics/BehaviorGraphTrace.h"
#include "Diagnostics/BehaviorExecutionTrace.h"
#include "Diagnostics/ScoreArrayTrace.h"
#include "Diagnostics/ExtraStateTrace.h"
#include "Diagnostics/VisualStateTrace.h"
#include "Diagnostics/PixelStateTrace.h"
#include "Game/RecordedCounterBoundary.h"

static RecordedCounterBoundary g_RecordedCounterBoundary;
static PixelStateTrace g_PixelStateTrace;

// Recording deltas are simulation data, independent of Player's wall pacing
// and ordinary 132-sample score lockstep. Keep retail clock semantics for the
// whole TAS level, including its score-conversion animation after playback.
static bool SetRecordedToolboxClock(IBML *bml, bool enabled) {
#ifdef TAS_SOURCE_ENGINE
    using SetClock = int (*)(CKContext *, CKBOOL);
    // Engine plugins can be RTLD_LOCAL. Resolve through the owning plugin,
    // rather than assuming its API is present in the process-global scope.
    auto *plugins = CKGetPluginManager();
    auto *entry = plugins ? plugins->FindComponent(CKGUID(0x60632E28, 0x7D3B3C7D)) : nullptr;
    auto *dll = entry ? plugins->GetPluginDllInfo(entry->m_PluginDllIndex) : nullptr;
    auto setClock = dll ? reinterpret_cast<SetClock>(dll->GetFunctionPtr(
        const_cast<char *>("TT_TimeSetRecordedClock"))) : nullptr;
    if (!bml || !setClock || setClock(bml->GetCKContext(), enabled ? TRUE : FALSE) != CK_OK)
        return false;
    g_RecordedCounterBoundary.Apply(bml, enabled);
    return true;
#else
    (void)bml;
    (void)enabled;
    return true; // Original Windows toolbox already uses the recorded delta.
#endif
}

template <typename Event>
static void PublishEngineEvent(BallanceTAS *mod, const Event &event) {
    if (!mod) {
        return;
    }

    auto *engine = mod->GetEngine();
    auto *eventBus = mod->GetEventBus();
    if (engine && eventBus && !engine->IsShuttingDown()) {
        eventBus->Publish(event);
    }
}

namespace {
ScoreArrayTrace g_ScoreArrayTrace;
ExtraStateTrace g_ExtraStateTrace;
VisualStateTrace g_VisualStateTrace;
VisualStateTrace g_RenderedVisualStateTrace{"BALLANCE_RENDERED_VISUAL_STATE_TRACE"};
bool ForceNativePhysicsForOracle() {
    const char *value = std::getenv("BALLANCE_TAS_FORCE_NATIVE_PHYSICS");
    return value && value[0] == '1' && value[1] == '\0';
}

#ifdef TAS_SOURCE_ENGINE
using PracticeAction = int (*)();

PracticeAction ResolvePracticeAction(const char *symbol) {
#ifdef __APPLE__
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char *image = _dyld_get_image_name(i);
        if (!image || !std::strstr(image, "PracticeTools.bmodp"))
            continue;
        if (void *handle = dlopen(image, RTLD_LAZY | RTLD_NOLOAD)) {
            auto action = reinterpret_cast<PracticeAction>(dlsym(handle, symbol));
            dlclose(handle);
            if (action) return action;
        }
    }
#else
    (void)symbol; // Practice provider integration follows playback qualification.
#endif
    return nullptr;
}
#endif

struct RegressionCoreState {
    float position[3]{};
    float angles[3]{};
    float velocity[3]{};
    float angularVelocity[3]{};
    int points = 0;
    int lives = 0;
    int sector = 0;
    bool ballValid = false;
};

RegressionCoreState CaptureRegressionCore(GameInterface *game) {
    RegressionCoreState state;
    if (!game) return state;
    state.points = game->GetPoints();
    state.lives = game->GetLifeCount();
    state.sector = game->GetCurrentSector();
    CK3dEntity *ball = game->GetActiveBall();
    PhysicsObject *physics = ball ? game->GetPhysicsObject(ball) : nullptr;
    if (!physics) return state;
    VxVector position, angles, velocity, angularVelocity;
    physics->GetPosition(&position, &angles);
    physics->GetVelocity(&velocity, &angularVelocity);
    std::memcpy(state.position, &position, sizeof(state.position));
    std::memcpy(state.angles, &angles, sizeof(state.angles));
    std::memcpy(state.velocity, &velocity, sizeof(state.velocity));
    std::memcpy(state.angularVelocity, &angularVelocity, sizeof(state.angularVelocity));
    state.ballValid = true;
    return state;
}

bool SameRegressionCore(const RegressionCoreState &a, const RegressionCoreState &b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void RunTasStateTrace(BallanceTAS *mod) {
#pragma pack(push, 1)
    struct TraceHeader {
        char magic[8];
        uint32_t version;
        uint32_t recordSize;
    };
#pragma pack(pop)
    static bool initialized = false;
    static FILE *trace = nullptr;
    if (!initialized) {
        initialized = true;
        const char *path = std::getenv("BALLANCE_TAS_STATE_TRACE");
        if (path && *path) {
            trace = std::fopen(path, "wb");
            if (!trace) {
                Log::Error("Could not open TAS state trace: %s", path);
            } else {
                const TraceHeader header{{'B', 'A', 'L', 'T', 'R', 'C', 'E', '\0'}, 2u, 344u};
                if (std::fwrite(&header, sizeof(header), 1, trace) != 1) {
                    Log::Error("Could not write TAS state trace header: %s", path);
                    std::fclose(trace);
                    trace = nullptr;
                }
            }
        }
    }
    if (!trace || !mod || !mod->GetEngine()) return;
    auto *playback = mod->GetEngine()->GetServiceProvider().Resolve<PlaybackService>();
    if (!playback || !playback->IsPlaying() ||
        playback->GetPlaybackType() != PlaybackType::Record) return;

#pragma pack(push, 1)
    struct TraceRecord {
        uint64_t tick;
        float deltaTimeMs;
        float deltaTimeFreeMs;
        float srScore;
        int32_t ballId;
        uint32_t keyBits;
        float position[3];
        float quaternion[4];
        float worldMatrix[16];
        float physicsAverageTickMs;
        double physicsTimeSeconds;
        double physicsNextPsiSeconds;
        double physicsLastTimeSeconds;
        uint32_t physicsTick;
        uint32_t coreValid;
        uint32_t coreMovementState;
        double coreTimeOfLastPsi;
        double corePosition[4];
        float coreSpeed[4];
        float coreRotSpeed[4];
        float coreSpeedChange[4];
        float coreRotSpeedChange[4];
        float coreDeltaPosition[4];
        double coreQuaternionLast[4];
        double coreQuaternionNext[4];
    } record{};
#pragma pack(pop)
    static_assert(sizeof(TraceRecord) == 344, "state trace record ABI must match the retail oracle");
    record.tick = playback->GetCurrentTick();
    auto *game = mod->GetEngine()->GetGameInterface();
    auto *time = game ? game->GetTimeManager() : nullptr;
    if (time) {
        record.deltaTimeMs = time->GetLastDeltaTime();
        record.deltaTimeFreeMs = time->GetLastDeltaTimeFree();
    }
    record.srScore = game ? game->GetSRScore() : 0.0f;
    CK3dEntity *ball = game ? game->GetActiveBall() : nullptr;
    CK3dEntity *tracedObject = ball;
    if (game) {
        const char *traceName = std::getenv("BALLANCE_TAS_TRACE_OBJECT");
        if (traceName && *traceName) {
            // Virtools permits duplicate object names. For collision-oracle
            // traces, select the requested instance nearest an optional world
            // position instead of silently tracing the first name-table hit.
            const char *nearText = std::getenv("BALLANCE_TAS_TRACE_OBJECT_NEAR");
            VxVector nearPoint{};
            const bool haveNear = nearText &&
                std::sscanf(nearText, "%f,%f,%f", &nearPoint.x, &nearPoint.y, &nearPoint.z) == 3;
            CK3dEntity *best = nullptr;
            float bestDistance2 = std::numeric_limits<float>::infinity();
            CKObject *previous = nullptr;
            CKContext *context = mod->GetBML()->GetCKContext();
            static bool candidatesDumped = false;
            const bool dumpCandidates = !candidatesDumped &&
                std::getenv("BALLANCE_TAS_TRACE_OBJECT_DUMP") != nullptr;
            bool sawCandidate = false;
            while (context && (previous = context->GetObjectByNameAndParentClass(
                       traceName, CKCID_3DENTITY, previous))) {
                sawCandidate = true;
                auto *candidate = static_cast<CK3dEntity *>(previous);
                float distance2 = 0.0f;
                if (haveNear) {
                    VxVector position{};
                    PhysicsObject *candidatePhysics = game->GetPhysicsObject(candidate);
#if defined(TAS_SOURCE_ENGINE)
                    IVP_Core *candidateCore = candidatePhysics && candidatePhysics->m_RealObject
                        ? candidatePhysics->m_RealObject->get_core() : nullptr;
                    if (candidateCore) {
                        position.x = static_cast<float>(candidateCore->pos_world_f_core_last_psi.k[0]);
                        position.y = static_cast<float>(candidateCore->pos_world_f_core_last_psi.k[1]);
                        position.z = static_cast<float>(candidateCore->pos_world_f_core_last_psi.k[2]);
                    } else {
                        candidate->GetPosition(&position);
                    }
#else
                    (void) candidatePhysics;
                    candidate->GetPosition(&position);
#endif
                    const float dx = position.x - nearPoint.x;
                    const float dy = position.y - nearPoint.y;
                    const float dz = position.z - nearPoint.z;
                    distance2 = dx * dx + dy * dy + dz * dz;
                    if (dumpCandidates) {
                        std::fprintf(stderr,
                            "TAS trace candidate id=%u name=%s core=(%.9g,%.9g,%.9g) distance2=%.9g\n",
                            static_cast<unsigned>(candidate->GetID()),
                            candidate->GetName() ? candidate->GetName() : "(null)",
                            position.x, position.y, position.z, distance2);
                    }
                }
                if (!best || distance2 < bestDistance2) {
                    best = candidate;
                    bestDistance2 = distance2;
                }
            }
            if (dumpCandidates && sawCandidate) candidatesDumped = true;
            if (best) tracedObject = best;
        }
    }
#if defined(TAS_SOURCE_ENGINE)
    CK3dEntity *secondaryObject = tracedObject;
#endif
    // Diagnostic control: keep the named-selector work identical while changing
    // only which core is exported. This isolates selector side effects from
    // core-state reads; it is not a prerequisite for deterministic playback.
    if (const char *playerCore = std::getenv("BALLANCE_TAS_TRACE_PLAYER_CORE")) {
        if (std::strcmp(playerCore, "1") == 0) tracedObject = ball;
    }
    if (ball) {
        record.ballId = static_cast<int32_t>(ball->GetID());
        VxVector position{};
        VxQuaternion quaternion{};
        ball->GetPosition(&position);
        ball->GetQuaternion(&quaternion);
        std::memcpy(record.position, &position, sizeof(record.position));
        record.quaternion[0] = quaternion.x;
        record.quaternion[1] = quaternion.y;
        record.quaternion[2] = quaternion.z;
        record.quaternion[3] = quaternion.w;
        const VxMatrix &matrix = ball->GetWorldMatrix();
        std::memcpy(record.worldMatrix, &matrix, sizeof(record.worldMatrix));
    }
    if (auto *input = game ? game->GetInputManager() : nullptr) {
        if (auto *keys = input->GetKeyboardState()) {
            const CKKEYBOARD watched[] = {
                CKKEY_UP, CKKEY_DOWN, CKKEY_LEFT, CKKEY_RIGHT,
                CKKEY_LSHIFT, CKKEY_SPACE, CKKEY_Q, CKKEY_ESCAPE, CKKEY_RETURN
            };
            for (uint32_t i = 0; i < sizeof(watched) / sizeof(watched[0]); ++i)
                if (keys[watched[i]]) record.keyBits |= (1u << i);
        }
    }
#if defined(TAS_SOURCE_ENGINE)
    if (auto *manager = game ? game->GetIpionManager() : nullptr) {
        record.physicsAverageTickMs = manager->GetDeltaTime();
        if (manager->GetEnvironment()) {
            CKIpionManager::SimulationClockState clock{};
            manager->CaptureSimulationClock(clock);
            record.physicsTimeSeconds = clock.currentTime;
            record.physicsNextPsiSeconds = clock.nextPsiTime;
            record.physicsLastTimeSeconds = clock.lastPsiTime;
            record.physicsTick = static_cast<uint32_t>(clock.currentTimeCode);
        }
        auto copyCoreFields = [](IVP_Core *core, TraceRecord &record) {
            if (!core) return;
            record.coreValid = 1;
            record.coreMovementState = static_cast<uint32_t>(core->movement_state);
            record.coreTimeOfLastPsi = core->time_of_last_psi.get_time();
            std::memcpy(record.corePosition, core->pos_world_f_core_last_psi.k, sizeof(record.corePosition));
            std::memcpy(record.coreSpeed, core->speed.k, sizeof(record.coreSpeed));
            std::memcpy(record.coreRotSpeed, core->rot_speed.k, sizeof(record.coreRotSpeed));
            std::memcpy(record.coreSpeedChange, core->speed_change.k, sizeof(record.coreSpeedChange));
            std::memcpy(record.coreRotSpeedChange, core->rot_speed_change.k, sizeof(record.coreRotSpeedChange));
            std::memcpy(record.coreDeltaPosition, core->delta_world_f_core_psis.k, sizeof(record.coreDeltaPosition));
            std::memcpy(record.coreQuaternionLast, &core->q_world_f_core_last_psi, sizeof(record.coreQuaternionLast));
            std::memcpy(record.coreQuaternionNext, &core->q_world_f_core_next_psi, sizeof(record.coreQuaternionNext));
        };
        auto copyCore = [manager, &copyCoreFields](CK3dEntity *entity, TraceRecord &record) {
            PhysicsObject *object = entity ? manager->GetPhysicsObject(entity) : nullptr;
            copyCoreFields(object && object->m_RealObject ? object->m_RealObject->get_core() : nullptr, record);
        };
        // The retail observer selects CurrentLevel's ball and scans the movable
        // object vector. A newly registered or dormant core may be absent there.
        // Keep this separate from the primary trace's broader hash-table view;
        // retain every TAS cursor row, including preplay and creation records.
        static bool retailInitialized = false;
        static FILE *retailTrace = nullptr;
        if (!retailInitialized) {
            retailInitialized = true;
            const char *path = std::getenv("BALLANCE_TAS_RETAIL_STATE_TRACE");
            if (path && *path) {
                const char *primaryPath = std::getenv("BALLANCE_TAS_STATE_TRACE");
                const char *secondaryPath = std::getenv("BALLANCE_TAS_SECONDARY_CORE_TRACE");
                if ((primaryPath && std::strcmp(path, primaryPath) == 0) ||
                    (secondaryPath && std::strcmp(path, secondaryPath) == 0)) {
                    Log::Error("Retail TAS trace must use a distinct path.");
                } else {
                    retailTrace = std::fopen(path, "wb");
                    const TraceHeader header{{'B', 'A', 'L', 'T', 'R', 'C', 'E', '\0'}, 2u, 344u};
                    if (!retailTrace || std::fwrite(&header, sizeof(header), 1, retailTrace) != 1) {
                        Log::Error("Could not initialize retail TAS trace: %s", path);
                        if (retailTrace) std::fclose(retailTrace);
                        retailTrace = nullptr;
                    }
                }
            }
        }
        if (retailTrace) {
            TraceRecord retail{};
            retail.tick = record.tick;
            retail.deltaTimeMs = record.deltaTimeMs;
            retail.deltaTimeFreeMs = record.deltaTimeFreeMs;
            retail.srScore = record.srScore;
            retail.keyBits = record.keyBits;
            retail.physicsAverageTickMs = record.physicsAverageTickMs;
            retail.physicsTimeSeconds = record.physicsTimeSeconds;
            retail.physicsNextPsiSeconds = record.physicsNextPsiSeconds;
            retail.physicsLastTimeSeconds = record.physicsLastTimeSeconds;
            retail.physicsTick = record.physicsTick;
            CKDataArray *level = mod->GetBML()->GetArrayByName("CurrentLevel");
            auto *retailBall = level ? static_cast<CK3dEntity *>(level->GetElementObject(0, 1)) : nullptr;
            const bool eligible = mod->GetBML()->IsPlaying() && retailBall;
            static int lastEligible = -1;
            if (lastEligible != static_cast<int>(eligible)) {
                std::fprintf(stderr,
                    "TAS_RETAIL_GATE tick=%llu eligible=%d playing=%d array_ball=%u active_ball=%u\n",
                    static_cast<unsigned long long>(record.tick), static_cast<int>(eligible),
                    static_cast<int>(mod->GetBML()->IsPlaying()),
                    retailBall ? static_cast<unsigned>(retailBall->GetID()) : 0u,
                    ball ? static_cast<unsigned>(ball->GetID()) : 0u);
                lastEligible = static_cast<int>(eligible);
            }
            if (retailBall) {
                retail.ballId = static_cast<int32_t>(retailBall->GetID());
                VxVector position{};
                VxQuaternion quaternion{};
                retailBall->GetPosition(&position);
                retailBall->GetQuaternion(&quaternion);
                std::memcpy(retail.position, &position, sizeof(retail.position));
                retail.quaternion[0] = quaternion.x;
                retail.quaternion[1] = quaternion.y;
                retail.quaternion[2] = quaternion.z;
                retail.quaternion[3] = quaternion.w;
                std::memcpy(retail.worldMatrix, &retailBall->GetWorldMatrix(), sizeof(retail.worldMatrix));
                for (int i = 0; i < manager->m_MovableObjects.len(); ++i) {
                    IVP_Real_Object *object = manager->m_MovableObjects.element_at(i);
                    if (object && object->client_data == retailBall) {
                        copyCoreFields(object->get_core(), retail);
                        break;
                    }
                }
            }
            if (eligible) g_ScoreArrayTrace.Gameplay(mod->GetBML(), record.tick);
            if (std::fwrite(&retail, sizeof(retail), 1, retailTrace) != 1) {
                Log::Error("Retail TAS trace write failed at frame %zu.", playback->GetCurrentTick());
                std::fclose(retailTrace);
                retailTrace = nullptr;
            } else if ((record.tick & 0xffu) == 0) {
                std::fflush(retailTrace);
            }
        }
        // Optional diagnostic stream: retain the player-core control while
        // recording the named prop. No simulation or entity transform calls.
        static bool secondaryInitialized = false;
        static FILE *secondaryTrace = nullptr;
        if (!secondaryInitialized) {
            secondaryInitialized = true;
            const char *path = std::getenv("BALLANCE_TAS_SECONDARY_CORE_TRACE");
            if (path && *path) {
                const char *primaryPath = std::getenv("BALLANCE_TAS_STATE_TRACE");
                if (primaryPath && std::strcmp(path, primaryPath) == 0) {
                    Log::Error("Secondary TAS trace must use a different path.");
                } else {
                    secondaryTrace = std::fopen(path, "wb");
                    const TraceHeader header{{'B', 'A', 'L', 'T', 'R', 'C', 'E', '\0'}, 2u, 344u};
                    if (!secondaryTrace || std::fwrite(&header, sizeof(header), 1, secondaryTrace) != 1) {
                        Log::Error("Could not initialize secondary TAS trace: %s", path);
                        if (secondaryTrace) std::fclose(secondaryTrace);
                        secondaryTrace = nullptr;
                    }
                }
            }
        }
        if (secondaryTrace) {
            TraceRecord secondaryRecord = record; // Core fields are still zero.
            copyCore(secondaryObject, secondaryRecord);
            // Diagnose selector/lifecycle changes without changing the trace ABI.
            PhysicsObject *selectedPhysics = secondaryObject
                ? manager->GetPhysicsObject(secondaryObject) : nullptr;
            IVP_Real_Object *selectedReal = selectedPhysics ? selectedPhysics->m_RealObject : nullptr;
            IVP_Core *selectedCore = selectedReal ? selectedReal->get_core() : nullptr;
            const CK_ID selectedId = secondaryObject ? secondaryObject->GetID() : 0;
            static CK_ID lastId = static_cast<CK_ID>(-1);
            static const void *lastReal = nullptr, *lastCore = nullptr;
            if (selectedId != lastId || selectedReal != lastReal || selectedCore != lastCore) {
                std::fprintf(stderr,
                    "TAS_SECONDARY_ID tick=%llu env=%a entity=%u name=%s object=%p core=%p valid=%u time=%a pos=(%a %a %a)\n",
                    static_cast<unsigned long long>(record.tick), record.physicsTimeSeconds,
                    static_cast<unsigned>(selectedId),
                    secondaryObject && secondaryObject->GetName() ? secondaryObject->GetName() : "(null)",
                    static_cast<void *>(selectedReal), static_cast<void *>(selectedCore),
                    secondaryRecord.coreValid, secondaryRecord.coreTimeOfLastPsi,
                    secondaryRecord.corePosition[0], secondaryRecord.corePosition[1], secondaryRecord.corePosition[2]);
                lastId = selectedId; lastReal = selectedReal; lastCore = selectedCore;
            }
            if (std::fwrite(&secondaryRecord, sizeof(secondaryRecord), 1, secondaryTrace) != 1) {
                Log::Error("Secondary TAS trace write failed at frame %zu.", playback->GetCurrentTick());
                std::fclose(secondaryTrace);
                secondaryTrace = nullptr;
            } else if ((record.tick & 0xffu) == 0) {
                std::fflush(secondaryTrace);
            }
        }
        copyCore(tracedObject, record);
    }
#else
    if (auto *manager = game ? game->GetIpionManager() : nullptr) {
        float factor = 0.0f;
        float physicsDelta = 0.0f;
        float psi = 0.0f;
        PhysicsRuntimeGetTiming(manager, &factor, &record.physicsAverageTickMs,
                                &physicsDelta, &psi, &record.physicsTimeSeconds);
        PhysicsObject *object = tracedObject ? manager->GetPhysicsObject(tracedObject) : nullptr;
        PhysicsRuntimeCoreState core{};
        if (object && PhysicsRuntimeGetCoreState(object, core)) {
            record.coreValid = 1;
            record.coreMovementState = static_cast<uint32_t>(core.movementState);
            record.coreTimeOfLastPsi = core.timeOfLastPsi;
            std::memcpy(record.corePosition, core.position, sizeof(record.corePosition));
            std::memcpy(record.coreSpeed, core.speed, sizeof(record.coreSpeed));
            std::memcpy(record.coreRotSpeed, core.rotSpeed, sizeof(record.coreRotSpeed));
            std::memcpy(record.coreSpeedChange, core.speedChange, sizeof(record.coreSpeedChange));
            std::memcpy(record.coreRotSpeedChange, core.rotSpeedChange, sizeof(record.coreRotSpeedChange));
            std::memcpy(record.coreDeltaPosition, core.deltaPosition, sizeof(record.coreDeltaPosition));
            std::memcpy(record.coreQuaternionLast, core.quaternionLast, sizeof(record.coreQuaternionLast));
            std::memcpy(record.coreQuaternionNext, core.quaternionNext, sizeof(record.coreQuaternionNext));
        }
    }
#endif
    if (std::fwrite(&record, sizeof(record), 1, trace) != 1) {
        Log::Error("TAS state trace write failed at frame %zu.",
                   playback->GetCurrentTick());
        std::fclose(trace);
        trace = nullptr;
    } else if ((record.tick & 0xffu) == 0) {
        std::fflush(trace);
    }
}

#ifdef TAS_SOURCE_ENGINE
void RunSnapshotSequenceAudit(BallanceTAS *mod) {
    struct Action { char kind; size_t tick; };
    struct Identity { CK_ID id; CKObject *pointer; CK_CLASSID cid; std::string name; };
    static bool initialized = false, failed = false;
    static std::vector<Action> actions;
    static std::vector<Identity> savedRenderOwners;
    static size_t next = 0, savedTick = 0;
    static bool hasSave = false;
    static RegressionCoreState savedCore;
    if (!initialized) {
        initialized = true;
        const char *program = std::getenv("BALLANCE_TAS_SNAPSHOT_SEQUENCE");
        while (program && *program) {
            char kind = 0;
            size_t tick = 0;
            int consumed = 0;
            if (std::sscanf(program, "%c%zu%n", &kind, &tick, &consumed) != 2 ||
                (kind != 's' && kind != 'r' && kind != 'd') || consumed <= 0 ||
                (program[consumed] && program[consumed] != ',')) {
                Log::Error("Snapshot sequence invalid program.");
                failed = true;
                break;
            }
            actions.push_back({kind, tick});
            program += consumed;
            if (*program == ',') ++program;
        }
        if (!actions.empty())
            Log::Info("Snapshot sequence configured actions=%zu.", actions.size());
    }
    if (failed || next >= actions.size() || !mod || !mod->GetEngine()) return;
    auto *playback = mod->GetEngine()->GetServiceProvider().Resolve<PlaybackService>();
    if (!playback || !playback->IsPlaying() ||
        playback->GetPlaybackType() != PlaybackType::Record) return;
    const size_t tick = playback->GetCurrentTick();
    const Action action = actions[next];
    if (tick < action.tick) return;
    if (tick != action.tick) {
        Log::Error("Snapshot sequence missed action=%zu expected=%zu actual=%zu.",
                   next, action.tick, tick);
        failed = true;
        return;
    }
    if (action.kind == 's') {
        PracticeAction capture = ResolvePracticeAction("BML_PracticeCaptureState");
        if (!capture || !capture()) {
            Log::Error("Snapshot sequence capture failed action=%zu tick=%zu.", next, tick);
            failed = true;
            return;
        }
        savedTick = tick;
        savedCore = CaptureRegressionCore(mod->GetEngine()->GetGameInterface());
        hasSave = true;
        if (std::getenv("BALLANCE_TAS_AUDIT_OBJECT_LIFETIMES")) {
            savedRenderOwners.clear();
            CKContext *context = mod->GetBML()->GetCKContext();
            const auto &objects = context->GetObjectListByType(CKCID_OBJECT, TRUE);
            for (auto *object : objects) {
                if (object && (CKIsChildClassOf(object, CKCID_RENDEROBJECT) ||
                    CKIsChildClassOf(object, CKCID_MATERIAL) || CKIsChildClassOf(object, CKCID_MESH)))
                    savedRenderOwners.push_back({object->GetID(), object, object->GetClassID(),
                        object->GetName() ? object->GetName() : "<unnamed>"});
            }
            Log::Info("Snapshot sequence owner audit captured=%zu.", savedRenderOwners.size());
        }
    } else if (action.kind == 'd') {
        // Exercise the normal game graph, as DebugUtilities' kill command does.
        // Do not delete the ball/UI objects or forge a life decrement here.
        CKBehavior *ingame = mod->GetBML()->GetScriptByName("Gameplay_Ingame");
        CKBehavior *manager = ingame ? ScriptHelper::FindFirstBB(ingame, "BallManager") : nullptr;
        CKBehavior *deactivate = manager ? ScriptHelper::FindFirstBB(manager, "Deactivate Ball") : nullptr;
        if (!deactivate || !mod->GetBML()->IsPlaying()) {
            Log::Error("Snapshot sequence death trigger unavailable action=%zu tick=%zu.", next, tick);
            failed = true;
            return;
        }
        deactivate->ActivateInput(0);
        deactivate->Activate();
    } else {
        CKContext *context = mod->GetBML()->GetCKContext();
        for (const auto &owner : savedRenderOwners) {
            if (context->GetObject(owner.id) != owner.pointer)
                Log::Info("Snapshot sequence missing owner id=%u class=%d name=%s.",
                    unsigned(owner.id), int(owner.cid), owner.name.c_str());
        }
        PracticeAction restore = ResolvePracticeAction("BML_PracticeRestoreState");
        if (!hasSave || !restore || !restore() || !playback->RewindRecordTo(savedTick)) {
            Log::Error("Snapshot sequence restore failed action=%zu tick=%zu.", next, tick);
            failed = true;
            return;
        }
        if (!SameRegressionCore(savedCore,
                CaptureRegressionCore(mod->GetEngine()->GetGameInterface()))) {
            Log::Error("Snapshot sequence restored core differs action=%zu.", next);
            failed = true;
            return;
        }
    }
    Log::Info("Snapshot sequence action=%zu kind=%c tick=%zu saved=%zu.",
              next, action.kind, tick, savedTick);
    if (++next == actions.size())
        Log::Info("Snapshot sequence completed actions=%zu.", next);
}

void RunSnapshotTasAudit(BallanceTAS *mod) {
    static bool initialized = false;
    static bool enabled = false;
    static size_t saveTick = 0;
    static size_t window = 0;
    static bool captured = false;
    static bool replaying = false;
    static RegressionCoreState savedCore;
    static std::vector<RegressionCoreState> baseline;
    if (!initialized) {
        initialized = true;
        const char *value = std::getenv("BALLANCE_TAS_SNAPSHOT_AUDIT");
        if (value && std::sscanf(value, "%zu:%zu", &saveTick, &window) == 2 && window > 0)
            enabled = true;
    }
    if (!enabled || !mod || !mod->GetEngine()) return;
    auto *playback = mod->GetEngine()->GetServiceProvider().Resolve<PlaybackService>();
    if (!playback || !playback->IsPlaying() ||
        playback->GetPlaybackType() != PlaybackType::Record) return;
    const size_t tick = playback->GetCurrentTick();
    if (!captured && tick == saveTick) {
        PracticeAction capture = ResolvePracticeAction("BML_PracticeCaptureState");
        if (!capture || !capture()) {
            Log::Error("Snapshot TAS audit could not capture at frame %zu.", tick);
            enabled = false;
            return;
        }
        captured = true;
        savedCore = CaptureRegressionCore(mod->GetEngine()->GetGameInterface());
        baseline.clear();
        baseline.reserve(window);
        Log::Info("Snapshot TAS audit captured at frame %zu; comparison window=%zu.", tick, window);
        return;
    }
    if (!captured) return;
    if (!replaying) {
        if (tick > saveTick && tick <= saveTick + window)
            baseline.push_back(CaptureRegressionCore(mod->GetEngine()->GetGameInterface()));
        if (tick == saveTick + window) {
            PracticeAction restore = ResolvePracticeAction("BML_PracticeRestoreState");
            if (!restore || !restore() || !playback->RewindRecordTo(saveTick)) {
                Log::Error("Snapshot TAS audit could not restore/rewind at frame %zu.", tick);
                enabled = false;
                return;
            }
            const RegressionCoreState restoredCore =
                CaptureRegressionCore(mod->GetEngine()->GetGameInterface());
            if (!SameRegressionCore(restoredCore, savedCore)) {
                Log::Error("Snapshot TAS audit restore point already differs at frame %zu: "
                           "pos %.9g %.9g %.9g expected %.9g %.9g %.9g; "
                           "vel %.9g %.9g %.9g expected %.9g %.9g %.9g.",
                           saveTick, restoredCore.position[0], restoredCore.position[1],
                           restoredCore.position[2], savedCore.position[0], savedCore.position[1],
                           savedCore.position[2], restoredCore.velocity[0], restoredCore.velocity[1],
                           restoredCore.velocity[2], savedCore.velocity[0], savedCore.velocity[1],
                           savedCore.velocity[2]);
            } else {
                Log::Info("Snapshot TAS audit restore point is bit-identical before resuming simulation.");
            }
            replaying = true;
            Log::Info("Snapshot TAS audit restored frame %zu after advancing to %zu.", saveTick, tick);
        }
        return;
    }
    if (tick <= saveTick) return;
    const size_t index = tick - saveTick - 1;
    if (index >= baseline.size()) return;
    const RegressionCoreState actual = CaptureRegressionCore(mod->GetEngine()->GetGameInterface());
    if (!SameRegressionCore(actual, baseline[index])) {
        Log::Error("Snapshot TAS audit diverged at replay frame %zu (offset %zu): "
                   "pos %.9g %.9g %.9g expected %.9g %.9g %.9g; "
                   "vel %.9g %.9g %.9g expected %.9g %.9g %.9g; score %d/%d sector %d/%d.",
                   tick, index + 1, actual.position[0], actual.position[1], actual.position[2],
                   baseline[index].position[0], baseline[index].position[1], baseline[index].position[2],
                   actual.velocity[0], actual.velocity[1], actual.velocity[2],
                   baseline[index].velocity[0], baseline[index].velocity[1], baseline[index].velocity[2],
                   actual.points, baseline[index].points, actual.sector, baseline[index].sector);
        enabled = false;
    } else if (index + 1 == baseline.size()) {
        Log::Info("Snapshot TAS audit PASS: %zu consecutive frames reproduced bit-for-bit.", baseline.size());
        enabled = false;
    }
}

// Opt-in diagnostic: repeat a completed record through the normal menu flow.
// In contrast to the rewind audit below, this must not insert an ordinary
// ResetLevel before exiting: those extra physics steps change inherited state.
void RunSameProcessTasAudit(BallanceTAS *mod) {
    enum class Phase { First, Exit, Main, Start, Second, Done };
    static Phase phase = Phase::First;
    static bool seenPlayback = false;
    static int level = [] {
        const char *value = std::getenv("BALLANCE_TAS_SAME_PROCESS_AUDIT");
        if (!value || !*value) return 0;
        char *end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        return end && *end == '\0' && parsed >= 1 && parsed <= 13
            ? static_cast<int>(parsed) : 0;
    }();
    if (!level || phase == Phase::Done || !mod || !mod->GetEngine()) return;
    auto *engine = mod->GetEngine();
    auto *game = engine->GetGameInterface();
    auto *playback = engine->GetServiceProvider().Resolve<PlaybackService>();
    if (!game || !playback) return;
    const bool playing = playback->IsPlaying() && playback->GetPlaybackType() == PlaybackType::Record;
    if (phase == Phase::First || phase == Phase::Second) {
        if (playing) {
            if (!seenPlayback)
                Log::Info("Same-process TAS audit playback %d started.", phase == Phase::First ? 1 : 2);
            seenPlayback = true;
            return;
        }
        if (!seenPlayback || !engine->IsIdle()) return;
        if (phase == Phase::Second) {
            Log::Info("Same-process TAS audit second playback stopped; requesting normal exit.");
            phase = Phase::Done;
            mod->GetBML()->ExitGame();
            return;
        }
        phase = Phase::Exit;
    }
    std::string error;
    const auto menu = game->GetActiveMenu();
    if (phase == Phase::Exit) {
        // Wait until the actual game-over/end button is ready; don't synthesize
        // a pause or level-reset event, and don't alter the stopped record.
        if ((menu == "dead" || menu == "end") && game->ClickMenuButton("main", error)) {
            Log::Info("Same-process TAS audit left first level via %s menu.", menu.c_str());
            phase = Phase::Main;
        }
    } else if (phase == Phase::Main && menu == "main") {
        if (game->ClickMenuButton("start", error)) phase = Phase::Start;
    } else if (phase == Phase::Start && menu == "start") {
        if (!engine->IsPendingPlay() && !engine->StartReplay()) {
            Log::Error("Same-process TAS audit failed to arm the same project.");
            phase = Phase::Done;
            return;
        }
        if (game->LoadLevel(level, error)) {
            seenPlayback = false;
            phase = Phase::Second;
            Log::Info("Same-process TAS audit loading second playback, level %d.", level);
        } else {
            Log::Error("Same-process TAS audit failed to load: %s", error.c_str());
            phase = Phase::Done;
        }
    }
}

// A complete runtime rewind can be reconstructed from the level-load boundary
// and its deterministic input stream.  This is both an oracle for the in-place
// snapshot implementation and the fallback for IVP states whose solver-owned
// topology cannot be copied safely (friction manifolds, mindists and constraint
// islands).  Unlike a tolerance check, this compares every observed float bit.
void RunReplayTasAudit(BallanceTAS *mod) {
    enum class Phase { Capture, Restarting, Replay, Done };
    static bool initialized = false;
    static bool enabled = false;
    static size_t saveTick = 0;
    static size_t window = 0;
    static Phase phase = Phase::Capture;
    static bool restartArmed = false;
    static std::vector<RegressionCoreState> baseline;
    if (!initialized) {
        initialized = true;
        const char *value = std::getenv("BALLANCE_TAS_REPLAY_AUDIT");
        if (value && std::sscanf(value, "%zu:%zu", &saveTick, &window) == 2 && window > 0)
            enabled = true;
    }
    if (!enabled || phase == Phase::Done || !mod || !mod->GetEngine()) return;
    auto *engine = mod->GetEngine();
    auto *playback = engine->GetServiceProvider().Resolve<PlaybackService>();
    if (phase == Phase::Restarting &&
        (!playback || !playback->IsPlaying())) {
        if (!restartArmed) {
            restartArmed = engine->RestartCurrentProject();
            if (restartArmed) {
                std::string error;
                GameInterface *game = engine->GetGameInterface();
                if (!game || !game->SendGameFlowEvent(
                                 GameInterface::GameFlowEvent::ResetLevel, error)) {
                    Log::Error("Replay TAS audit could not reset the level: %s",
                               error.c_str());
                    phase = Phase::Done;
                }
            } else {
                Log::Warn("Replay TAS audit is waiting for the TAS session to become idle.");
            }
        }
        return;
    }
    if (!playback || !playback->IsPlaying() ||
        playback->GetPlaybackType() != PlaybackType::Record) return;
    const size_t tick = playback->GetCurrentTick();

    if (phase == Phase::Restarting) {
        // The next active record playback is the fresh level-load run.
        phase = Phase::Replay;
        Log::Info("Replay TAS audit second run activated; target frame=%zu.", saveTick);
    }

    if (phase == Phase::Capture) {
        if (tick >= saveTick && tick <= saveTick + window)
            baseline.push_back(CaptureRegressionCore(engine->GetGameInterface()));
        if (tick != saveTick + window) return;

        Log::Info("Replay TAS audit captured frames %zu..%zu; restarting from level load.",
                  saveTick, saveTick + window);
        phase = Phase::Restarting;
        engine->StopReplay(false);
        return;
    }

    if (phase != Phase::Replay || tick < saveTick) return;
    const size_t index = tick - saveTick;
    if (index >= baseline.size()) return;
    const RegressionCoreState actual = CaptureRegressionCore(engine->GetGameInterface());
    if (!SameRegressionCore(actual, baseline[index])) {
        const RegressionCoreState &expected = baseline[index];
        Log::Error("Replay TAS audit diverged at frame %zu (offset %zu): "
                   "pos %.9g %.9g %.9g expected %.9g %.9g %.9g; "
                   "vel %.9g %.9g %.9g expected %.9g %.9g %.9g; score %d/%d sector %d/%d.",
                   tick, index, actual.position[0], actual.position[1], actual.position[2],
                   expected.position[0], expected.position[1], expected.position[2],
                   actual.velocity[0], actual.velocity[1], actual.velocity[2],
                   expected.velocity[0], expected.velocity[1], expected.velocity[2],
                   actual.points, expected.points, actual.sector, expected.sector);
        phase = Phase::Done;
    } else if (index + 1 == baseline.size()) {
        Log::Info("Replay TAS audit PASS: level-load reconstruction reproduced %zu states bit-for-bit.",
                  baseline.size());
        phase = Phase::Done;
    }
}
#endif
}

// Global instance pointer required by BML
BallanceTAS *g_Mod;

namespace {
// Commands are looked up for each UI action, never cached by other mods. The
// concrete command objects live for this module's lifetime and are unregistered
// before unloading. Passing a vector avoids reparsing filenames as shell text.
class TasEntryCommand final : public ICommand {
public:
    explicit TasEntryCommand(bool menu) : m_Menu(menu) {}
    std::string GetName() override { return m_Menu ? "tasmenu" : "tasplay"; }
    std::string GetAlias() override { return {}; }
    std::string GetDescription() override {
        return m_Menu ? "Open TAS projects." : "Prepare a TAS record by project name or path.";
    }
    bool IsCheat() override { return false; }
    const std::vector<std::string> GetTabCompletion(IBML *, const std::vector<std::string> &) override { return {}; }
    void Execute(IBML *bml, const std::vector<std::string> &args) override {
        auto fail = [bml](const std::string &message) {
            if (bml) bml->SendIngameMessage(message.c_str());
            Log::Error("TAS entry: %s", message.c_str());
            BML_SetCommandStatus(1);
        };
        auto *engine = g_Mod && g_Mod->GetBML() == bml ? g_Mod->GetEngine() : nullptr;
        if (!engine || engine->IsShuttingDown()) {
            fail("Enable BallanceTAS before opening a TAS project."); return;
        }
        try {
            if (m_Menu) {
                if (engine->GetGameInterface()->IsIngame() || !g_Mod->GetUIManager()) {
                    fail("Open TAS projects from the level selection menu."); return;
                }
                auto *start = bml->GetScriptByName("Menu_Start");
                auto *exit = start ? ScriptHelper::FindFirstBB(start, "Exit") : nullptr;
                if (!exit) { fail("The level selection menu is not available."); return; }
                g_Mod->GetUIManager()->OpenTASMenu();
                if (!g_Mod->GetUIManager()->IsTASMenuOpen()) {
                    fail("The TAS project menu could not be opened."); return;
                }
                exit->ActivateInput(0); exit->Activate();
                Log::Info("TAS entry: opened project menu.");
            } else {
                if (args.size() != 2 || args[1].empty()) {
                    fail("Usage: tasplay <record name or path>"); return;
                }
                if (!engine->IsIdle() && !engine->IsPendingPlay()) {
                    fail("Stop the current TAS operation before selecting another record."); return;
                }
                auto *projects = engine->GetServiceProvider().Resolve<ProjectManager>();
                std::string error;
                auto *project = projects ? projects->ResolveLaunchTarget(args[1], error) : nullptr;
                if (!project || !project->IsValid() || !project->IsRecordProject()) {
                    fail(error.empty() ? "Select a valid TAS record." : error); return;
                }
                if (engine->IsPendingPlay()) engine->StopReplay(false);
                projects->SetCurrentProject(project);
                if (!engine->StartReplay()) {
                    projects->SetCurrentProject(nullptr);
                    fail("The TAS record could not be prepared."); return;
                }
                bml->SendIngameMessage("TAS record prepared. Start its level to play it.");
                Log::Info("TAS entry: prepared record '%s'.", project->GetName().c_str());
            }
            BML_SetCommandStatus(0);
        } catch (const std::exception &error) { fail(error.what()); }
    }
private:
    bool m_Menu;
};
TasEntryCommand g_TasMenuCommand(true), g_TasPlayCommand(false);

void RemoveTasEntryCommands(IBML *bml) {
    if (!bml) return;
    for (auto *command : {&g_TasMenuCommand, &g_TasPlayCommand}) {
        const auto name = command->GetName();
        if (bml->FindCommand(name.c_str()) == command) {
            const int result = BML_UnregisterCommand(name.c_str());
            if (result != BML_OK) Log::Error("Failed to unregister %s: %d", name.c_str(), result);
        }
    }
}
}

#ifdef TAS_SOURCE_ENGINE
// Optional inter-mod contract: physical-key reconciliation must not overwrite
// the input stream owned by replay/editing. Query rather than cache ownership,
// since playback can stop, pause, restart or unload within the same level.
extern "C" __attribute__((visibility("default"))) int BML_TASOwnsKeyboard(
    CKInputManager *input) {
    auto *engine = g_Mod ? g_Mod->GetEngine() : nullptr;
    auto *game = engine ? engine->GetGameInterface() : nullptr;
    return input && game && game->GetInputManager() == input &&
        (engine->IsPlaying() || engine->IsPaused() || engine->IsTranslating());
}
#endif

// BML entry and exit points
MOD_EXPORT IMod *BMLEntry(IBML *bml) {
    g_Mod = new BallanceTAS(bml);
    return g_Mod;
}

MOD_EXPORT void BMLExit(IMod *mod) {
    delete static_cast<BallanceTAS *>(mod);
    g_Mod = nullptr;
}

BallanceTAS::BallanceTAS(IBML *bml) : IMod(bml) {
    m_Logger = IMod::GetLogger();
}

BallanceTAS::~BallanceTAS() {
    RemoveTasEntryCommands(m_BML);
    PhysicsRuntimeSetSolverMode(false);
    // Ensure shutdown is called even if OnUnload isn't (e.g., forceful exit)
    if (m_Initialized) {
        Shutdown();
    }

    // Shutdown global logger system (safe to call multiple times)
    Log::Shutdown();

    m_Logger = nullptr;
}

void BallanceTAS::OnLoad() {
    // Initialize global logger system via the BML adapter
    m_LogSink = std::make_unique<BMLLogSink>(GetLogger());
    Log::Initialize(m_LogSink.get());

    // --- 1. Initialize Configuration ---
    m_ConfigService.RegisterProperties(GetConfig());

    for (auto *command : {&g_TasMenuCommand, &g_TasPlayCommand}) {
        if (!m_BML->FindCommand(command->GetName().c_str())) m_BML->RegisterCommand(command);
        else Log::Error("Command already registered: %s", command->GetName().c_str());
    }

    m_InputManager = m_BML->GetInputManager();

    InitPhysicsAddresses();

#ifdef _WIN32
    // Initialize MinHook
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Log::Error("MinHook failed to initialize: %s", MH_StatusToString(status));
    }
#endif

    m_EventBus = std::make_unique<EventBus>();
    m_HookManager = std::make_unique<HookManager>();

    if (!InitializeGameHooks()) {
        Log::Error("Failed to initialize shared game hooks.");
    }

    // --- 2. Initialize TAS Framework if enabled ---
    if (m_ConfigService.IsEnabled() || HarnessRuntime::IsRequested() || PlayerHostBridge::IsRequested()) {
        if (!Initialize()) {
            Log::Error("Failed to initialize BallanceTAS framework.");
            // Framework is disabled due to initialization failure
        }
    }
}

void BallanceTAS::OnUnload() {
    m_ExplosionPolicy.Forget(); // CK world may already have been destroyed.
    // The host may have destroyed the world before unloading mods. Sampling
    // belongs to live Process/PreExitLevel callbacks, not final file cleanup.
    g_ScoreArrayTrace.Reset();
    g_RecordedCounterBoundary.Forget(); // No world access during teardown.
    RemoveTasEntryCommands(m_BML);
    BehaviorExecutionTrace::Stop([this](CK_ID id) { return m_BML->GetCKContext()->GetObject(id); });
    // Never leave the host in the expensive TAS arithmetic mode after the mod
    // is disabled or unloaded.
    PhysicsRuntimeSetSolverMode(false);
    // Shutdown framework
    Shutdown();

    DisableGameHooks();
    m_HookManager.reset();
    m_EventBus.reset();

#ifdef _WIN32
    MH_Uninitialize();
#endif

    // Shutdown global logger system
    Log::Shutdown();
}

void BallanceTAS::OnModifyConfig(const char *category, const char *key, IProperty *prop) {
    // Notify ConfigService (publishes ConfigChangedEvent on the EventBus).
    m_ConfigService.OnPropertyChanged(category, key, prop);

    // Handle enable/disable toggle directly since it controls initialization lifecycle.
    if (std::string_view(category) == "TAS" && std::string_view(key) == "Enable") {
        if (m_ConfigService.IsEnabled() && !m_Initialized) {
            Log::Info("BallanceTAS framework enabled.");
            if (!Initialize()) {
                Log::Error("Failed to enable BallanceTAS framework.");
            }
        } else if (!m_ConfigService.IsEnabled() && m_Initialized) {
            Log::Info("BallanceTAS framework disabled.");
            SetRecordedToolboxClock(m_BML, false);
            Shutdown();
        }
        return;
    }

    if (!m_Initialized) return;

    // Sync settings that need direct forwarding to subsystems.
    if (std::string_view(category) == "TAS") {
        if (m_Engine) {
            m_Engine->SetValidationEnabled(m_ConfigService.IsValidation());
            m_Engine->SetAutoRestartEnabled(m_ConfigService.IsAutoRestart());
            m_Engine->SetPreserveLevelHistory(m_ConfigService.PreserveLevelHistory());
        }
    } else if (std::string_view(category) == "Recording") {
        if (auto *recorder = m_Engine ? m_Engine->GetServiceProvider().Resolve<Recorder>() : nullptr) {
            recorder->SetMaxFrames(m_ConfigService.GetMaxFrames());
        }
    } else if (std::string_view(category) == "Hotkeys") {
        if (m_UIManager) {
            m_UIManager->SetStopHotkey(m_ConfigService.GetStopKey());
        }
    } else if (std::string_view(category) == "OSD") {
        SetOSDVisible(m_ConfigService.IsOSDVisible());
        UpdateOSDPanelConfig();
    }
}

void BallanceTAS::OnLoadObject(const char *filename, CKBOOL isMap, const char *masterName, CK_CLASSID filterClass,
                               CKBOOL addToScene, CKBOOL reuseMeshes, CKBOOL reuseMaterials, CKBOOL dynamic,
                               XObjectArray *objArray, CKObject *masterObj) {
    if (std::getenv("BALLANCE_TAS_OBJECT_INVENTORY") && filename &&
        (!std::strcmp(filename, "base.cmo") || !std::strcmp(filename, "3D Entities\\Language.nmo"))) {
        auto *context = m_BML->GetCKContext();
        const unsigned tick = static_cast<unsigned>(m_BML->GetTimeManager()->GetMainTickCount());
        const int count = context->GetObjectCount();
        GetLogger()->Info("OBJECT_INVENTORY tick=%u count=%d file=%s", tick, count, filename);
        for (int id = 1; id <= count; ++id) {
            auto *object = context->GetObject(id);
            GetLogger()->Info("OBJECT_INVENTORY_ITEM id=%d present=%d class=%d flags=%08x name=%s", id,
                object != nullptr, object ? object->GetClassID() : 0,
                object ? static_cast<unsigned>(object->GetObjectFlags()) : 0,
                object && object->GetName() ? object->GetName() : "<null>");
        }
        const auto &freeIds = context->m_ObjectManager->m_FreeObjectIDs;
        GetLogger()->Info("OBJECT_FREE_IDS count=%d valid=1", freeIds.Size());
        for (int i = 0; i < freeIds.Size(); ++i)
            GetLogger()->Info("OBJECT_FREE_ID index=%u id=%u", static_cast<unsigned>(i), static_cast<unsigned>(freeIds[i]));
    }
    if (std::getenv("BALLANCE_TAS_LOAD_CATALOG")) {
        auto *context = m_BML->GetCKContext();
        const unsigned tick = static_cast<unsigned>(m_BML->GetTimeManager()->GetMainTickCount());
        GetLogger()->Info("LOAD_CATALOG tick=%u count=%d global=%d file=%s", tick,
            objArray ? objArray->Size() : 0, context->GetObjectCount(), filename ? filename : "<null>");
        for (int i = 0; objArray && i < objArray->Size(); ++i) {
            auto *object = context->GetObject((*objArray)[i]);
            GetLogger()->Info("LOAD_CATALOG_ITEM index=%d id=%u class=%d name=%s", i,
                static_cast<unsigned>((*objArray)[i]), object ? object->GetClassID() : 0,
                object && object->GetName() ? object->GetName() : "<null>");
        }
    }
    if (m_Initialized && m_Engine) {
        if (!strcmp(filename, "3D Entities\\Gameplay.nmo")) {
            m_Engine->GetGameInterface()->AcquireGameplayInfo();
        }

        if (isMap) {
            std::string mapName = filename;
            mapName = mapName.substr(mapName.find_last_of('\\') + 1);
            mapName = mapName.substr(0, mapName.find_last_of('.'));
            m_Engine->GetGameInterface()->SetMapName(mapName);
        }
    }

    if (!strcmp(filename, "3D Entities\\Menu.nmo")) {
        m_Level01 = m_BML->Get2dEntityByName("M_Start_But_01");
        CKBehavior *menuStart = m_BML->GetScriptByName("Menu_Start");
        m_ExitStart = ScriptHelper::FindFirstBB(menuStart, "Exit");
        CKBehavior *menuMain = m_BML->GetScriptByName("Menu_Main");
        m_ExitMain = ScriptHelper::FindFirstBB(menuMain, "Exit", false, 1, 0);
    }
}

void BallanceTAS::OnPhysicalize(CK3dEntity *target, CKBOOL fixed, float, float, float mass,
                               const char *, CKBOOL frozen, CKBOOL collision, CKBOOL,
                               float linearDamp, float rotDamp, const char *, VxVector,
                               int, CKMesh **, int, VxVector *, float *, int, CKMesh **) {
#if defined(TAS_SOURCE_ENGINE)
    if (target) m_LegacyPreload.Register(target->GetID(), target, target->GetName());
#endif
    if (!std::getenv("BALLANCE_TAS_PHYSICALIZE_ORDER") || !target) return;
    GetLogger()->Info("PHYS_ORDER add tick=%u id=%u name=%s fixed=%d frozen=%d collision=%d mass=%a linear=%a angular=%a",
        static_cast<unsigned>(GetBML()->GetTimeManager()->GetMainTickCount()),
        static_cast<unsigned>(target->GetID()), target->GetName() ? target->GetName() : "<null>",
        fixed, frozen, collision, mass, linearDamp, rotDamp);
}

void BallanceTAS::OnUnphysicalize(CK3dEntity *target) {
#if defined(TAS_SOURCE_ENGINE)
    if (target) m_LegacyPreload.Retire(target->GetID(), target,
        [this](const std::vector<LegacyPreloadSession::Owner> &captured) -> std::string {
            using Complete = decltype(&TT_PhysicsCompleteFreefallPreloadV1);
            // CK loads component DLLs in a local symbol scope. Resolve through
            // the registered Physics Manager component, not RTLD_DEFAULT or an
            // unrelated module with the same filename.
            auto *plugins = CKGetPluginManager();
            auto *entry = plugins ? plugins->FindComponent(CKGUID(0x6bed328b, 0x141f5148)) : nullptr;
            auto *dll = entry ? plugins->GetPluginDllInfo(entry->m_PluginDllIndex) : nullptr;
            auto complete = dll ? reinterpret_cast<Complete>(dll->GetFunctionPtr(
                const_cast<char *>("TT_PhysicsCompleteFreefallPreloadV1"))) : nullptr;
            if (!complete) return "The physics runtime does not support legacy preload completion.";
            auto *context = GetBML()->GetCKContext();
            std::vector<CK3dEntity *> owners;
            owners.reserve(captured.size());
            for (const auto &owner : captured) {
                auto *object = context->GetObject(owner.id);
                if (!object || object != owner.identity || !CKIsChildClassOf(object->GetClassID(), CKCID_3DENTITY))
                    return "A startup object was replaced before initialization finished.";
                owners.push_back(static_cast<CK3dEntity *>(object));
            }
            auto *manager = context->GetManagerByGuid(CKGUID(0x6bed328b, 0x141f5148));
            TT_PhysicsPreloadResultV1 result{};
            result.size = sizeof(result);
            const bool success = complete(manager, owners.data(), static_cast<unsigned>(owners.size()), &result) != 0;
            GetLogger()->Info("Legacy preload: status=%d objects=%u steps=%u time=%a->%a seed=%d->%d countdown=%d->%d",
                result.status, result.objects, result.steps, result.timeBefore, result.timeAfter,
                result.seedBefore, result.seedAfter, result.countdownBefore, result.countdownAfter);
            if (!success) return "Startup physics could not be initialized reproducibly (status " + std::to_string(result.status) + ").";
            return {};
        });
#endif
    if (std::getenv("BALLANCE_TAS_PHYSICALIZE_ORDER") && target)
        GetLogger()->Info("PHYS_ORDER remove tick=%u id=%u name=%s",
            static_cast<unsigned>(GetBML()->GetTimeManager()->GetMainTickCount()),
            static_cast<unsigned>(target->GetID()), target->GetName() ? target->GetName() : "<null>");
#if defined(TAS_SOURCE_ENGINE)
    const char *enabled = std::getenv("BALLANCE_TAS_LIFECYCLE_TRACE");
    if (!enabled || enabled[0] != '1' || !target || !target->GetName()
        || std::strncmp(target->GetName(), "P_Box_MF", 8) != 0) return;
    auto *manager = static_cast<CKIpionManager *>(GetBML()->GetCKContext()->GetManagerByGuid(CKGUID(0x6bed328b, 0x141f5148)));
    auto *time = GetBML()->GetTimeManager();
    if (!manager || !manager->GetEnvironment() || !time) return;
    GetLogger()->Info("PHYS_UNPHYS host=%u time=%a dt=%a id=%u name=%s next_check=%d seed=%d",
        static_cast<unsigned>(time->GetMainTickCount()),
        manager->GetEnvironment()->get_current_time().get_time(), time->GetLastDeltaTime(),
        static_cast<unsigned>(target->GetID()), target->GetName(),
        static_cast<int>(manager->GetNextMovementCheck()), ivp_srand_read());
#else
    (void)target;
#endif
}

void BallanceTAS::OnLoadScript(const char *filename, CKBehavior *script) {
    BehaviorExecutionTrace::Install(script);
    m_ExplosionPolicy.Register(script);
    if (m_Initialized && m_Engine) {
        if (!strcmp(script->GetName(), "Gameplay_Ingame")) {
            for (int i = 0; i < script->GetLocalParameterCount(); ++i) {
                CKParameter *param = script->GetLocalParameter(i);
                if (!strcmp(param->GetName(), "ActiveBall")) {
                    m_Engine->GetGameInterface()->SetActiveBall(param);
                    break;
                }
            }
        }
    }
}

bool BallanceTAS::InitializeGameHooks() {
    if (m_GameHooksEnabled) {
        Log::Warn("Game hooks already enabled.");
        return true;
    }

    Log::Info("Enabling game hooks...");

    CKContext *context = GetBML()->GetCKContext();
    if (!context) {
        Log::Error("Could not get CKContext to find managers.");
        return false;
    }

    if (!m_HookManager) {
        Log::Error("HookManager is not available.");
        return false;
    }

    bool success = true;

    try {
        auto *timeManager = (CKTimeManager *) context->GetManagerByGuid(TIME_MANAGER_GUID);
        if (!timeManager || !m_HookManager->EnableTimeManagerHook(timeManager)) {
            Log::Error("Failed to enable TimeManager hook.");
            success = false;
        } else {
            Log::Info("TimeManager hook enabled.");
        }

        auto *inputManager = (CKInputManager *) context->GetManagerByGuid(INPUT_MANAGER_GUID);
        if (!inputManager || !m_HookManager->EnableInputManagerHook(inputManager)) {
            Log::Error("Failed to enable InputManager hook.");
            success = false;
        } else {
            Log::Info("InputManager hook enabled.");
            m_IdleScriptInputGuard = m_HookManager->RegisterPostInputCallback(
                [this](CKInputManager *im) { ApplyIdleScriptInput(im); });
        }

        auto *ipionManager = context->GetManagerByGuid(CKGUID(0x6bed328b, 0x141f5148));
        if (!ipionManager || !PhysicsDeterminism::EnableFpuMonitor(ipionManager)) {
            // Not fatal: only diagnostics are lost.
            Log::Warn("Failed to enable physics FPU monitor.");
        } else {
            Log::Info("Physics FPU monitor enabled.");
        }
    } catch (const std::exception &e) {
        Log::Error("Exception enabling hooks: %s", e.what());
        success = false;
    }

    if (success) {
        m_GameHooksEnabled = true;
        Log::Info("Game hooks enabled successfully.");
    } else {
        // Cleanup partial success
        DisableGameHooks();
    }

    return success;
}

void BallanceTAS::ApplyIdleScriptInput(CKInputManager *inputManager) {
    // Only when no TAS is armed or running: PlaybackService owns the keyboard (and the played
    // script's key timing) from the moment a playback is armed, and recordings must stay pure.
    if (!inputManager || !m_Engine || !m_Engine->IsIdle()) {
        return;
    }
    auto *contexts = m_Engine->GetServiceProvider().Resolve<ScriptContextManager>();
    if (!contexts) {
        return;
    }

    std::vector<InputSystem *> inputs;
    for (const auto &context : contexts->GetContextsByPriority()) {
        if (context && context->IsExecuting()) {
            InputSystem *input = context->GetInputSystem();
            if (input && input->IsEnabled()) {
                inputs.push_back(input);
            }
        }
    }
    if (!inputs.empty()) {
        InputSystem::OverlayMergedOnKeyboardState(m_Engine->GetCurrentTick(), inputs,
                                                  inputManager->GetKeyboardState());
    }
}

void BallanceTAS::DisableGameHooks() {
    if (!m_GameHooksEnabled) return;

    Log::Info("Disabling game hooks...");

    try {
        m_IdleScriptInputGuard.Reset();
        if (m_HookManager) {
            m_HookManager->DisableAll();
        }
        PhysicsDeterminism::DisableFpuMonitor();

        Log::Info("Game hooks disabled.");
    } catch (const std::exception &e) {
        Log::Error("Exception disabling hooks: %s", e.what());
    }

    m_GameHooksEnabled = false;
}

bool BallanceTAS::Initialize() {
    if (m_Initialized) {
        Log::Warn("BallanceTAS framework already initialized.");
        return true;
    }

    Log::Info("Initializing BallanceTAS framework...");

    try {
        if (!m_GameHooksEnabled || !m_EventBus || !m_HookManager) {
            throw std::runtime_error("Shared runtime infrastructure is not available.");
        }

        m_GameInterface = std::make_unique<GameInterface>(this);

        // Initialize TAS Engine
        m_Engine = std::make_unique<TASEngine>(m_GameInterface.get(), m_EventBus.get(), m_HookManager.get());
#ifdef TAS_SOURCE_ENGINE
        // Native Ballanced keeps writable loader data below its --user-data-path.
        // The historical relative path contains Windows separators and would create
        // a literally named "..\\ModLoader\\TAS\\" directory on POSIX.
        if (const char *loaderPath = BML_GetLoaderPathUtf8(BML_DIR_LOADER)) {
            std::filesystem::path tasRoot = std::filesystem::path(loaderPath) / "TAS";
            std::string path = tasRoot.string();
            path.push_back(std::filesystem::path::preferred_separator);
            m_Engine->SetPath(path);
        }
#endif
        if (!m_Engine->Initialize()) {
            throw std::runtime_error("Engine failed to initialize.");
        }

        m_ConfigService.SetEventBus(m_Engine->GetEventBus());
        m_ProjectChoicesGeneration = 0;
        PollProjects();
        m_Engine->SetValidationEnabled(m_ConfigService.IsValidation());
        m_Engine->SetAutoRestartEnabled(m_ConfigService.IsAutoRestart());
        m_Engine->SetPreserveLevelHistory(m_ConfigService.PreserveLevelHistory());
        const bool hostRequested = PlayerHostBridge::IsRequested();
        const bool harnessRequested = !hostRequested && HarnessRuntime::IsRequested();
        if (!hostRequested && !harnessRequested) {
            if (auto *startup = m_Engine->GetServiceProvider().Resolve<StartupProjectManager>()) {
                StartupConfig startupConfig = m_ConfigService.GetStartupConfig();
                startup->SetStartupEnabled(startupConfig.enabled);
                startup->SetAutoLoadEnabled(startupConfig.autoLoad);
                if (!startupConfig.project.empty()) {
                    startup->SetStartupProject(startupConfig.project);
                }
                startup->SetStartupLevel(startupConfig.level);
                if (startupConfig.enabled && startupConfig.autoLoad) {
                    startup->LoadAndExecuteStartupScript();
                }
            }
        }

        if (hostRequested) {
            m_PlayerHostBridge = std::make_unique<PlayerHostBridge>(m_Engine.get());
            if (!m_PlayerHostBridge->Initialize()) {
                throw std::runtime_error("TAS Player host bridge failed to initialize.");
            }
        } else if (auto *harness = m_Engine->GetServiceProvider().Resolve<HarnessRuntime>()) {
            if (!harness->Initialize()) {
                throw std::runtime_error("Harness runtime failed to initialize.");
            }
        }

        // Initialize UI Manager
        m_UIManager = std::make_unique<UIManager>(m_Engine.get());
        if (!m_UIManager->Initialize()) {
            throw std::runtime_error("UIManager failed to initialize.");
        }
        m_UIManager->SetStopHotkey(m_ConfigService.GetStopKey());

        m_Initialized = true;
        Log::Info("BallanceTAS framework initialized successfully.");

        // Sync initial config states
        SetOSDVisible(m_ConfigService.IsOSDVisible());
        UpdateOSDPanelConfig();

        // Configure recording settings
        if (auto *recorder = m_Engine->GetServiceProvider().Resolve<Recorder>()) {
            recorder->SetMaxFrames(m_ConfigService.GetMaxFrames());
            recorder->SetAutoGenerate(true); // Always auto-generate
        }

        return true;
    } catch (const std::exception &e) {
        Log::Error("Exception during initialization: %s", e.what());

        // Clean up partial initialization
        if (m_UIManager) {
            m_UIManager->Shutdown();
            m_UIManager.reset();
        }
        if (m_PlayerHostBridge) {
            m_PlayerHostBridge->Shutdown();
            m_PlayerHostBridge.reset();
        }
        if (m_Engine) {
            m_Engine->Shutdown();
            m_Engine.reset();
        }
        if (m_GameInterface) {
            m_GameInterface.reset();
        }
        m_ConfigService.SetEventBus(nullptr);
        return false;
    }
}

void BallanceTAS::Shutdown() {
    m_ExplosionPolicy.Apply(m_BML->GetCKContext(), false);
    if (!m_Initialized) {
        PhysicsRuntimeSetSolverMode(false);
        return;
    }

    Log::Info("Shutting down BallanceTAS framework...");

    try {
        m_ConfigService.SetEventBus(nullptr);

        // Unregister from Player before any runtime service can be destroyed.
        if (m_PlayerHostBridge) {
            m_PlayerHostBridge->Shutdown();
            m_PlayerHostBridge.reset();
        }

        // Shutdown UI first
        if (m_UIManager) {
            m_UIManager->Shutdown();
            m_UIManager.reset();
        }

        // Then shutdown engine
        if (m_Engine) {
            m_Engine->Shutdown();
            m_Engine.reset();
        }

        Log::Info("BallanceTAS framework shutdown complete.");
    } catch (const std::exception &e) {
        Log::Error("Exception during shutdown: %s", e.what());
    }

    m_Initialized = false;
    // Disabling TAS in its settings does not unload the mod. Once OnProcess
    // stops servicing the framework, it can no longer release the precise
    // solver there. Do so after teardown, also when a service threw above.
    PhysicsRuntimeSetSolverMode(false);
    // NOTE: Determinism hooks remain active for fair gameplay
}

void BallanceTAS::OnMenuStart() {
    // The compatibility adapter owns the original CK entry button and delegates
    // to tasmenu. Keep a single clickable entry when that adapter is installed.
    auto *legacy = m_BML->FindMod("Segment");
    if (legacy && std::strcmp(legacy->GetName(), "Legacy TAS Runtime") == 0) return;
    if (m_Level01 && m_Level01->IsVisible()) {
        const ImVec2 &vpSize = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowPos(ImVec2(vpSize.x * 0.61f, vpSize.y * 0.88f));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        constexpr ImGuiWindowFlags ButtonFlags = ImGuiWindowFlags_NoDecoration |
                                                 ImGuiWindowFlags_NoBackground |
                                                 ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoNav |
                                                 ImGuiWindowFlags_AlwaysAutoResize |
                                                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                                 ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::Begin("Button_TAS", nullptr, ButtonFlags)) {
            if (Bui::SmallButton("TAS")) {
                m_ExitStart->ActivateInput(0);
                m_ExitStart->Activate();
                if (m_UIManager) {
                    m_UIManager->ToggleTASMenu();
                }
            }
        }

        ImGui::End();

        ImGui::PopStyleVar(2);
    }
}

void BallanceTAS::OnRender(CK_RENDER_FLAGS) {
    if (m_Initialized && m_Engine) {
        auto *playback=m_Engine->GetServiceProvider().Resolve<PlaybackService>();
        if (playback && playback->IsPlaying()) {
            g_VisualStateTrace.MarkRender(playback->GetCurrentTick());
            // IMod::OnRender precedes foreground 2D rendering. Observe only
            // after those sprites have updated their layout caches too.
            // Temporary callbacks are removed in the next PreProcess; the
            // observer does not become part of a practice snapshot.
            const char *path = std::getenv("BALLANCE_RENDERED_VISUAL_STATE_TRACE");
            const char *pixels = std::getenv("BALLANCE_PIXEL_TRACE_DIR");
            if (((path && *path) || (pixels && *pixels)) && m_BML->GetRenderContext()) {
                m_BML->GetRenderContext()->AddPostSpriteRenderCallBack(
                    [](CKRenderContext *context, void *argument) {
                        auto *self = static_cast<BallanceTAS *>(argument);
                        if (!self->m_Initialized || !self->m_Engine) return;
                        auto *current = self->m_Engine->GetServiceProvider().Resolve<PlaybackService>();
                        if (current && current->IsPlaying()) {
                            g_RenderedVisualStateTrace.Sample(self->m_BML, current->GetCurrentTick());
                            g_PixelStateTrace.Sample(context, current->GetCurrentTick());
                        }
                    }, this, TRUE);
            }
        }
    }
}

void BallanceTAS::OnProcess() {
    g_ScoreArrayTrace.Process(m_BML);
    if (m_Initialized && m_Engine) {
        auto *playback=m_Engine->GetServiceProvider().Resolve<PlaybackService>();
        if (playback && playback->IsPlaying()) {
            g_ExtraStateTrace.Sample(m_BML,playback->GetCurrentTick());
            g_VisualStateTrace.Sample(m_BML,playback->GetCurrentTick());
        }
    }
    if (m_Initialized && m_Engine && m_UIManager) {
        const bool tasTimeline = (
            m_Engine->IsPlaying() ||
            m_Engine->IsPendingPlay() ||
            m_Engine->IsRecording() ||
            m_Engine->IsPendingRecord() ||
            m_Engine->IsTranslating() ||
            m_Engine->IsPendingTranslate() ||
            (m_Engine->IsPaused() && m_Engine->GetPlaybackType() != PlaybackType::None));
        const bool precisePhysics = !ForceNativePhysicsForOracle() && tasTimeline;
        m_ExplosionPolicy.Apply(m_BML->GetCKContext(), tasTimeline);
        if (tasTimeline && !SetRecordedToolboxClock(m_BML, true)) {
            Log::Error("TAS requires a toolbox runtime with recorded-clock support.");
            Shutdown();
            return;
        }
        // Pre-load selection and framework shutdown also set this mode. Read
        // the runtime rather than retaining a request cache across lifetimes.
        bool currentPrecisePhysics = false;
        if ((!PhysicsRuntimeGetSolverMode(currentPrecisePhysics) ||
             currentPrecisePhysics != precisePhysics) &&
            PhysicsRuntimeSetSolverMode(precisePhysics)) {
            Log::Info("Physics solver mode: %s.",
                      precisePhysics ? "precise x87 (TAS)" : "native fast");
        }
        OnMenuStart();
        m_Engine->TickLevelReload();

        m_Engine->IncrementCurrentTick();
        PhysicsDeterminism::SetFpuMonitoring(
            m_Engine->IsPlaying() || m_Engine->IsPendingPlay() ||
            m_Engine->IsRecording() || m_Engine->IsPendingRecord() ||
            m_Engine->IsTranslating() || m_Engine->IsPendingTranslate());
        if (!m_Engine->DrivesScriptContexts()) {
            if (auto *contexts = m_Engine->GetServiceProvider().Resolve<ScriptContextManager>()) {
                contexts->TickAll();
            }
        }
        if (auto *startup = m_Engine->GetServiceProvider().Resolve<StartupProjectManager>()) {
            startup->Tick();
        }
        PollProjects();
        SkipRenderingToConfiguredFrame();
        if (auto *harness = m_Engine->GetServiceProvider().Resolve<HarnessRuntime>()) {
            harness->Tick();
        }
        if (m_PlayerHostBridge) {
            m_PlayerHostBridge->Tick();
        }
#ifdef TAS_SOURCE_ENGINE
        RunSnapshotSequenceAudit(this);
        RunSnapshotTasAudit(this);
        RunReplayTasAudit(this);
        RunSameProcessTasAudit(this);
#endif
        RunTasStateTrace(this);

        // Process and render UI
        m_UIManager->Process();
        m_UIManager->Render();
    }

    if (m_SkipRenderingCount != 0) {
        m_BML->SkipRenderForNextTick();
        --m_SkipRenderingCount;
    }
}

void BallanceTAS::SkipRenderingToConfiguredFrame() {
    const int target = m_ConfigService.GetSkipRenderUntilFrame();
    if (target <= 0 || !m_Engine->IsPlaying()) {
        return;
    }
    auto *playback = m_Engine->GetServiceProvider().Resolve<PlaybackService>();
    if (playback && playback->GetCurrentTick() < static_cast<size_t>(target)) {
        m_BML->SkipRenderForNextTick();
    }
}

void BallanceTAS::PollProjects() {
    constexpr size_t kPollIntervalFrames = 120;
    auto *projects = m_Engine->GetServiceProvider().Resolve<ProjectManager>();
    if (!projects) {
        return;
    }
    // Only between runs: a rescan reads new project files, which must not stall playback.
    if (++m_ProjectPollFrames >= kPollIntervalFrames && m_Engine->IsIdle()) {
        m_ProjectPollFrames = 0;
        projects->RefreshIfChanged();
    }
    // Any rescan counts, including the TAS menu's own refreshes.
    if (projects->GetGeneration() != m_ProjectChoicesGeneration) {
        m_ProjectChoicesGeneration = projects->GetGeneration();
        std::vector<std::string> names;
        for (const auto &project : projects->GetProjects()) {
            if (project && project->IsValid()) {
                names.push_back(project->GetName());
            }
        }
        m_ConfigService.SetStartupProjectChoices(names);
    }
}

void BallanceTAS::SetOSDVisible(bool visible) {
    if (m_Initialized && m_UIManager) {
        m_UIManager->SetOSDVisible(visible);
    }
}

void BallanceTAS::UpdateOSDPanelConfig() {
    if (!m_Initialized || !m_UIManager) return;

    auto *osd = m_UIManager->GetOSD();
    if (!osd) return;

    OSDConfig cfg = m_ConfigService.GetOSDConfig();

    osd->SetPanelVisible(OSDPanel::Status, cfg.showStatus);
    osd->SetPanelVisible(OSDPanel::Ball, cfg.showBall);
    osd->SetPanelVisible(OSDPanel::Input, cfg.showInput);
    osd->SetPanelVisible(OSDPanel::Validation, cfg.showValidation);
    osd->SetPanelVisible(OSDPanel::Watches, cfg.showWatches);

    osd->SetPosition(cfg.positionX, cfg.positionY);

    osd->SetOpacity(cfg.opacity);
    osd->SetScale(cfg.scale);

    Log::Info("OSD panel configuration updated.");
}

void BallanceTAS::SkipRenderingForTicks(size_t ticks) {
    m_SkipRenderingCount = ticks;
}

// --- Event Forwarding Implementations ---

void BallanceTAS::OnPreStartMenu() {
#if defined(TAS_SOURCE_ENGINE)
    m_LegacyPreload.FinishMenu();
    const auto error = m_LegacyPreload.Error();
    if (!error.empty()) GetLogger()->Error("%s", error.c_str());
#endif
    PublishEngineEvent(this, PreStartMenuEvent{});
}

void BallanceTAS::OnPostStartMenu() {
    PublishEngineEvent(this, PostStartMenuEvent{});
}

void BallanceTAS::OnExitGame() {
    m_Level01 = nullptr;
    if (m_EventBus) {
        m_EventBus->Publish(ExitGameEvent{});
    }
}

void BallanceTAS::OnPreLoadLevel() {
    g_ScoreArrayTrace.Reset();
    BehaviorExecutionTrace::objectLookup = [this](CK_ID id) { return m_BML->GetCKContext()->GetObject(id); };
    BehaviorExecutionTrace::Arm(m_BML->GetTimeManager(), m_BML->GetScriptByName("Default Level"));
    if (m_GameInterface) {
        m_GameInterface->StopLeftoverBallNavigation();
    }
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        // A record's initial state includes the level-loading physics.  Enter
        // the precise solver before those scripts run, not on the first input
        // sample, otherwise resting dynamic objects acquire a platform-native
        // pose before TAS playback begins.
        if (!SetRecordedToolboxClock(m_BML, m_Engine->LevelLoadRunsTAS())) {
            Log::Error("TAS level load requires a toolbox runtime with recorded-clock support.");
            Shutdown();
            return;
        }
        if (m_Engine->LevelLoadRunsTAS() && !ForceNativePhysicsForOracle()) {
            PhysicsRuntimeSetSolverMode(true);
            // The script owns the physics factor during loading and gameplay.
            // Keep any previous host compensation out of TAS simulation.
            m_Engine->GetGameInterface()->SetHostPhysicsTimeScale(1.0f);
        }
        m_Engine->GetGameInterface()->SetLevelCompleted(false);
        if (m_Engine->LevelLoadRunsTAS()) {
            // Before the level's own loading scripts start, so a TAS level load takes the
            // same number of frames every time.
            m_Engine->GetGameInterface()->NormalizeBehaviorOrder();
        }
        m_Engine->Start();
    }
    PublishEngineEvent(this, PreLoadLevelEvent{});
}

// Macro for trivial BML callback -> EventBus forwarding
#define FORWARD_SIMPLE_EVENT(BmlCallback, EventType) \
    void BallanceTAS::BmlCallback() { PublishEngineEvent(this, EventType{}); }

FORWARD_SIMPLE_EVENT(OnPostLoadLevel,   PostLoadLevelEvent)
void BallanceTAS::OnStartLevel() {
    BehaviorGraphTrace::Capture(m_BML);
    PublishEngineEvent(this, StartLevelEvent{});
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        m_Engine->OnLevelStartedForReload();
    }
}

void BallanceTAS::OnPreResetLevel() {
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        m_Engine->GetGameInterface()->SetLevelCompleted(false);
        PublishEngineEvent(this, PreResetLevelEvent{});
        m_Engine->Stop();
        m_Engine->ReloadInsteadOfRestart();
    }
}

FORWARD_SIMPLE_EVENT(OnPostResetLevel,  PostResetLevelEvent)
FORWARD_SIMPLE_EVENT(OnPauseLevel,      PauseLevelEvent)
FORWARD_SIMPLE_EVENT(OnUnpauseLevel,    UnpauseLevelEvent)

void BallanceTAS::OnPreExitLevel() {
    g_ScoreArrayTrace.End(m_BML);
    SetRecordedToolboxClock(m_BML, false);
    if (m_GameInterface) {
        m_GameInterface->TurnOffNavigation(m_BallNavActive, m_CamNavActive);
    }
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        PublishEngineEvent(this, PreExitLevelEvent{});
        m_Engine->Stop();
    }
}

FORWARD_SIMPLE_EVENT(OnPostExitLevel,   PostExitLevelEvent)
FORWARD_SIMPLE_EVENT(OnPreNextLevel,    PreNextLevelEvent)
FORWARD_SIMPLE_EVENT(OnPostNextLevel,   PostNextLevelEvent)
FORWARD_SIMPLE_EVENT(OnDead,            DeadEvent)
FORWARD_SIMPLE_EVENT(OnPreEndLevel,     PreEndLevelEvent)
FORWARD_SIMPLE_EVENT(OnPostEndLevel,    PostEndLevelEvent)
FORWARD_SIMPLE_EVENT(OnCounterActive,   CounterActiveEvent)
FORWARD_SIMPLE_EVENT(OnCounterInactive, CounterInactiveEvent)
void BallanceTAS::OnBallNavActive() {
    m_BallNavActive = true;
    PublishEngineEvent(this, BallNavActiveEvent{});
}

void BallanceTAS::OnBallNavInactive() {
    m_BallNavActive = false;
    PublishEngineEvent(this, BallNavInactiveEvent{});
}

void BallanceTAS::OnCamNavActive() {
    m_CamNavActive = true;
    PublishEngineEvent(this, CamNavActiveEvent{});
}

void BallanceTAS::OnCamNavInactive() {
    m_CamNavActive = false;
    PublishEngineEvent(this, CamNavInactiveEvent{});
}

FORWARD_SIMPLE_EVENT(OnBallOff,         BallOffEvent)
FORWARD_SIMPLE_EVENT(OnGameOver,        GameOverEvent)

#undef FORWARD_SIMPLE_EVENT

void BallanceTAS::OnPreCheckpointReached() {
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        int sector = -1;
        if (m_Engine->GetGameInterface()) {
            sector = m_Engine->GetGameInterface()->GetCurrentSector();
        }
        PublishEngineEvent(this, PreCheckpointReachedEvent{sector});
    }
}

void BallanceTAS::OnPostCheckpointReached() {
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        int sector = -1;
        if (m_Engine->GetGameInterface()) {
            sector = m_Engine->GetGameInterface()->GetCurrentSector();
        }
        PublishEngineEvent(this, PostCheckpointReachedEvent{sector});
    }
}

void BallanceTAS::OnLevelFinish() {
    g_ScoreArrayTrace.Finish(m_BML);
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        m_Engine->GetGameInterface()->SetLevelCompleted(true);
        PublishEngineEvent(this, LevelFinishEvent{});
        if (m_ConfigService.IsStopOnFinish()) {
            m_Engine->Stop();
        }
    }
}

void BallanceTAS::OnExtraPoint() {
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        int points = 0;
        if (m_Engine->GetGameInterface()) {
            points = m_Engine->GetGameInterface()->GetPoints();
        }
        PublishEngineEvent(this, ExtraPointEvent{points});
    }
}

void BallanceTAS::OnPreSubLife() {
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        int lifeCount = 0;
        if (m_Engine->GetGameInterface()) {
            lifeCount = m_Engine->GetGameInterface()->GetLifeCount();
        }
        PublishEngineEvent(this, PreSubLifeEvent{lifeCount});
    }
}

void BallanceTAS::OnPostSubLife() {
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        int lifeCount = 0;
        if (m_Engine->GetGameInterface()) {
            lifeCount = m_Engine->GetGameInterface()->GetLifeCount();
        }
        PublishEngineEvent(this, PostSubLifeEvent{lifeCount});
    }
}

void BallanceTAS::OnPreLifeUp() {
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        int lifeCount = 0;
        if (m_Engine->GetGameInterface()) {
            lifeCount = m_Engine->GetGameInterface()->GetLifeCount();
        }
        PublishEngineEvent(this, PreLifeUpEvent{lifeCount});
    }
}

void BallanceTAS::OnPostLifeUp() {
    if (m_Initialized && m_Engine && !m_Engine->IsShuttingDown()) {
        int lifeCount = 0;
        if (m_Engine->GetGameInterface()) {
            lifeCount = m_Engine->GetGameInterface()->GetLifeCount();
        }
        PublishEngineEvent(this, PostLifeUpEvent{lifeCount});
    }
}
