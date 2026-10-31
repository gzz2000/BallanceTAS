#include "GameInterface.h"

#include <BML/ScriptHelper.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

#include "LevelNames.h"

#include "Logger.h"
#include "BallanceTAS.h"
#include "UIManager.h"

template <typename T>
static bool SetDataArrayValue(CKDataArray *array, int row, int column, const T &value) {
    if (!array) {
        return false;
    }

    return array->SetElementValue(row, column, const_cast<T *>(&value), sizeof(T)) == CK_OK;
}

template <typename T>
static bool SetParameterValue(CKParameter *parameter, const T &value) {
    if (!parameter) {
        return false;
    }

    parameter->SetValue(&value, sizeof(T));
    return true;
}

// ========================================
// Construction & Destruction
// ========================================

GameInterface::GameInterface(BallanceTAS *mod) : m_Mod(mod) {
    if (!m_Mod) {
        throw std::runtime_error("GameInterface requires a valid BallanceTAS instance.");
    }

    m_BML = m_Mod->GetBML();
    if (!m_BML) {
        throw std::runtime_error("GameInterface requires a valid IBML instance.");
    }

    m_CKContext = m_BML->GetCKContext();
    if (!m_CKContext) {
        throw std::runtime_error("GameInterface requires a valid CKContext instance.");
    }

    m_RenderContext = m_BML->GetRenderContext();
    if (!m_RenderContext) {
        throw std::runtime_error("GameInterface requires a valid CKRenderContext instance.");
    }

    m_TimeManager = m_CKContext->GetTimeManager();
    if (!m_TimeManager) {
        throw std::runtime_error("GameInterface requires a valid CKTimeManager instance.");
    }

    m_InputManager = static_cast<CKInputManager *>(
        m_CKContext->GetManagerByGuid(INPUT_MANAGER_GUID));
    if (!m_InputManager) {
        throw std::runtime_error("GameInterface requires a valid CKInputManager instance.");
    }

    m_IpionManager = (CKIpionManager *) m_CKContext->GetManagerByGuid(CKGUID(0x6bed328b, 0x141f5148));
    if (!m_IpionManager) {
        throw std::runtime_error("GameInterface requires a valid CKIpionManager instance.");
    }
}

// ========================================
// Core & Manager Access
// ========================================

UIManager *GameInterface::GetUIManager() const {
    return m_Mod->GetUIManager();
}

std::string GameInterface::GetLegacyPreloadError() const {
    return m_Mod->GetLegacyPreloadError();
}

// ========================================
// Initialization & Configuration
// ========================================

void GameInterface::AcquireGameplayInfo() {
    // 3D Entities\Gameplay.nmo
    m_CurrentLevel = m_BML->GetArrayByName("CurrentLevel");
    m_Energy = m_BML->GetArrayByName("Energy");
    m_CheckPoints = m_BML->GetArrayByName("Checkpoints");
    m_IngameParam = m_BML->GetArrayByName("IngameParameter");
    CKBehavior *events = m_BML->GetScriptByName("Gameplay_Events");
    CKBehavior *id = ScriptHelper::FindNextBB(events, events->GetInput(0));
    m_CurrentSector = id->GetOutputParameter(0)->GetDestination(0);
}

void GameInterface::AcquireKeyBindings() {
    m_Keyboard = m_BML->GetArrayByName("Keyboard");
}

// ========================================
// Physics & Time Management
// ========================================

void GameInterface::ResetPhysicsTime() {
    ResetPhysicsTime(m_TimeManager ? m_TimeManager->GetLastDeltaTime() : 1000.0f / 132.0f);
}

void GameInterface::ResetPhysicsTime(float deltaTimeMs) {
    // Reset physics time in order to sync with TAS records
#if !defined(__APPLE__)
    if (PhysicsRuntimeBridgeActive() && m_TimeManager) {
        // The retail SDK header has no recovered Player-only fixed-clock
        // accessors.  The semantic physics trace records the effective delta
        // below, which is what matters for the reverse runtime.
        Log::Info("Reverse physics bridge timing: last delta %.9g ms.",
                  m_TimeManager->GetLastDeltaTime());
    }
#endif
#ifdef __APPLE__
    m_IpionManager->ResetSimulationClock();
#else
    if (PhysicsRuntimeBridgeActive()) {
        PhysicsRuntimeResetForTas(m_IpionManager, deltaTimeMs);
    } else {
    // IVP_Environment
    auto *env = m_IpionManager->GetEnvironment();

    auto &base_time = env->time_manager->base_time;
#ifdef _DEBUG
    Log::Info("time_manager->base_time: %f", base_time);
#endif
    base_time = 0;

    auto &current_time = env->current_time;
#ifdef _DEBUG
    Log::Info("current_time: %f", current_time);
#endif
    current_time = 0;

    auto &time_of_last_psi = env->time_of_last_psi;
#ifdef _DEBUG
    Log::Info("time_of_last_psi: %f", time_of_last_psi);
#endif
    time_of_last_psi = 0;

    auto &time_of_next_psi = env->time_of_next_psi;
#ifdef _DEBUG
    Log::Info("time_of_next_psi: %f", time_of_next_psi);
#endif
    time_of_next_psi = time_of_last_psi + 1.0 / 66;
    }
#endif

    if (m_TimeManager) {
        m_TimeManager->SetLastDeltaTime(deltaTimeMs);
    }
#ifdef __APPLE__
    // A canonical TAS reset starts with the record's fixed sample, including
    // its smoothing history. Blending with the menu delta leaves a permanent
    // clock phase error when a 132/240 Hz record follows the 264 Hz menu.
    // Match TT_PhysicsResetForTas; ordinary frame updates still use smoothing.
    m_IpionManager->SetDeltaTimeImmediate(deltaTimeMs);
#else
    if (!PhysicsRuntimeBridgeActive())
        m_IpionManager->SetDeltaTime(deltaTimeMs);
#endif
}

void GameInterface::SetHostPhysicsTimeScale(float scale) {
#ifdef __APPLE__
    if (m_TimeManager) {
        m_TimeManager->SetFixedPhysicsTimeScale(scale);
    }
#else
    (void) scale;
#endif
}

PhysicsDeterminism::PhysicsLevelBaseline GameInterface::CapturePhysicsLevelBaseline() const {
    PhysicsDeterminism::PhysicsLevelBaseline baseline;
#ifdef __APPLE__
    baseline.timeFactor = m_IpionManager->GetPhysicsTimeFactor();
    baseline.nextMovementCheck = m_IpionManager->GetNextMovementCheck();
#else
    if (PhysicsRuntimeBridgeActive()) {
        PhysicsRuntimeLevelBaseline runtime = {};
        if (PhysicsRuntimeCaptureLevelBaseline(m_IpionManager, runtime)) {
            baseline.timeFactor = runtime.timeFactor;
            baseline.nextMovementCheck = runtime.nextMovementCheck;
            baseline.randomSeed = runtime.randomSeed;
            return baseline;
        }
    } else {
        baseline.timeFactor = m_IpionManager->GetPhysicsTimeFactor();
        if (auto *env = m_IpionManager->GetEnvironment())
            baseline.nextMovementCheck = env->next_movement_check;
    }
#endif
    baseline.randomSeed = ivp_srand_read();
    return baseline;
}

void GameInterface::NormalizeBehaviorOrder() {
    CKBehaviorManager *behaviors = m_CKContext ? m_CKContext->GetBehaviorManager() : nullptr;
    if (!behaviors) {
        return;
    }
    XObjectPointerArray &objects = behaviors->m_BeObjects;
    std::vector<CKObject *> ordered(objects.Begin(), objects.End());
    std::stable_sort(ordered.begin(), ordered.end(), [](CKObject *a, CKObject *b) {
        const int pa = static_cast<CKBeObject *>(a)->GetPriority();
        const int pb = static_cast<CKBeObject *>(b)->GetPriority();
        return pa != pb ? pa > pb : a->GetID() < b->GetID();
    });
    for (size_t i = 0; i < ordered.size(); ++i) {
        objects[static_cast<int>(i)] = ordered[i];
    }
}

static void DeactivateGraph(CKBehavior *behavior) {
    for (int i = 0; i < behavior->GetSubBehaviorCount(); ++i) {
        DeactivateGraph(behavior->GetSubBehavior(i));
    }
    if (behavior->IsActive()) {
        behavior->Activate(FALSE);
    }
}

// Gameplay_Ingame is already stopped when the BallNav deactivate message TurnOffNavigation sends
// arrives, so do what the message would: forget "on" and switch the keys off (their forces
// went away with the old ball).
void GameInterface::StopLeftoverBallNavigation() {
    CKBehavior *ingame = m_BML->GetScriptByName("Gameplay_Ingame");
    if (!ingame) {
        return;
    }
    bool stopped = false;
    if (CKBehavior *onOff = ScriptHelper::FindFirstBB(ingame, "BallNav On/Off")) {
        for (int i = 0; i < onOff->GetSubBehaviorCount(); ++i) {
            CKBehavior *memory = onOff->GetSubBehavior(i);
            if (std::strcmp(memory->GetName(), "Binary Memory") != 0 || memory->GetOutputParameterCount() < 1) {
                continue;
            }
            CKBOOL on = FALSE;
            memory->GetOutputParameterValue(0, &on);
            if (on) {
                const CKBOOL off = FALSE;
                memory->SetOutputParameterValue(0, &off);
                stopped = true;
            }
        }
    }
    if (CKBehavior *navigation = ScriptHelper::FindFirstBB(ingame, "Ball Navigation")) {
        if (navigation->IsActive()) {
            DeactivateGraph(navigation);
            stopped = true;
        }
    }
    if (stopped) {
        Log::Info("Turned off the ball navigation an early level exit left on.");
    }
}

void GameInterface::TurnOffNavigation(bool ballNav, bool camNav) {
    CKMessageManager *messages = m_CKContext ? m_CKContext->GetMessageManager() : nullptr;
    if (!messages) {
        return;
    }
    if (ballNav) {
        messages->SendMessageBroadcast(messages->AddMessageType((CKSTRING) "BallNav deactivate"), CKCID_BEOBJECT);
    }
    if (camNav) {
        messages->SendMessageBroadcast(messages->AddMessageType((CKSTRING) "CamNav deactivate"), CKCID_BEOBJECT);
    }
}

void GameInterface::RestorePhysicsLevelBaseline(const PhysicsDeterminism::PhysicsLevelBaseline &baseline) {
#if !defined(__APPLE__)
    if (PhysicsRuntimeBridgeActive()) {
        const PhysicsRuntimeLevelBaseline runtime{
            baseline.timeFactor, baseline.nextMovementCheck, 0, baseline.randomSeed};
        PhysicsRuntimeRestoreLevelBaseline(m_IpionManager, runtime);
        return;
    }
#endif
    m_IpionManager->SetPhysicsTimeFactor(baseline.timeFactor);
#ifdef __APPLE__
    m_IpionManager->SetNextMovementCheck(baseline.nextMovementCheck);
#else
    if (auto *env = m_IpionManager->GetEnvironment())
        env->next_movement_check = baseline.nextMovementCheck;
#endif
    ivp_srand(baseline.randomSeed);
}

void GameInterface::SetPhysicsTimeFactor(float factor) {
    // Reset physics time factor in case it was changed
#if !defined(__APPLE__)
    if (PhysicsRuntimeBridgeActive()) {
        PhysicsRuntimeSetTimeFactor(m_IpionManager, factor * 0.001f);
        return;
    }
#endif
    m_IpionManager->SetPhysicsTimeFactor(factor * 0.001f);
}

// ========================================
// Input Management
// ========================================

CKKEYBOARD GameInterface::RemapKey(CKKEYBOARD key) const {
    if (!m_Keyboard) {
        // If no keyboard remapping array is available, return the original key
        return key;
    }

    // The keyboard array stores remapped keys in specific positions
    CKKEYBOARD remappedKey = key; // Default to original key

    switch (key) {
    case CKKEY_UP:
        m_Keyboard->GetElementValue(0, 0, &remappedKey);
        return remappedKey;
    case CKKEY_DOWN:
        m_Keyboard->GetElementValue(0, 1, &remappedKey);
        return remappedKey;
    case CKKEY_LEFT:
        m_Keyboard->GetElementValue(0, 2, &remappedKey);
        return remappedKey;
    case CKKEY_RIGHT:
        m_Keyboard->GetElementValue(0, 3, &remappedKey);
        return remappedKey;
    case CKKEY_LSHIFT:
        m_Keyboard->GetElementValue(0, 4, &remappedKey);
        return remappedKey;
    case CKKEY_SPACE:
        m_Keyboard->GetElementValue(0, 5, &remappedKey);
        return remappedKey;
    default:
        // For keys not in the remapping table, return original
        return key;
    }
}

// ========================================
// Object Access
// ========================================

CK3dEntity *GameInterface::GetActiveBall() const {
    if (m_ActiveBallParam) {
        CKObject *obj = m_ActiveBallParam->GetValueObject();
        return static_cast<CK3dEntity *>(obj);
    }
    return nullptr;
}

void GameInterface::SetActiveBall(CKParameter *param) {
    m_ActiveBallParam = param;
}

CKCamera *GameInterface::GetActiveCamera() const {
    return m_RenderContext->GetAttachedCamera();
}

CK3dEntity *GameInterface::GetObjectByName(const std::string &name) const {
    return m_BML->Get3dEntityByName(name.c_str());
}

CK3dEntity *GameInterface::GetObjectByID(int id) const {
    CKObject *obj = m_CKContext->GetObject(id);
    if (!obj) {
        return nullptr;
    }
    if (CKIsChildClassOf(obj,CKCID_3DENTITY)) {
        return static_cast<CK3dEntity *>(obj);
    }
    return nullptr;
}

PhysicsObject *GameInterface::GetPhysicsObject(CK3dEntity *entity) const {
    PhysicsObject *object = m_IpionManager ? m_IpionManager->GetPhysicsObject(entity) : nullptr;
    static bool timingLogged = false;
    if (object && !timingLogged) {
        timingLogged = true;
#ifdef __APPLE__
        Log::Info("Physics timing at first TAS object: factor %.9g, delta %.9g ms, "
                  "submitted %.9g s, PSI %.9g s, simulation %.12g s.",
                  m_IpionManager->GetPhysicsTimeFactor(), m_IpionManager->GetDeltaTime(),
                  m_IpionManager->GetPhysicsDeltaTime(),
                  m_IpionManager->GetSimulationTimeStep(),
                  m_IpionManager->GetSimulationTime().get_time());
#else
        if (PhysicsRuntimeBridgeActive()) {
            float factor = 0.0f, delta = 0.0f, submitted = 0.0f, psi = 0.0f;
            double simulation = 0.0;
            if (PhysicsRuntimeGetTiming(m_IpionManager, &factor, &delta, &submitted,
                                        &psi, &simulation)) {
                Log::Info("Physics timing at first TAS object: factor %.9g, delta %.9g ms, "
                          "submitted %.9g s, PSI %.9g s, simulation %.12g s.",
                          factor, delta, submitted, psi, simulation);
            }
        }
#endif
    }
    return object;
}

// ========================================
// Object Property Queries
// ========================================

VxVector GameInterface::GetPosition(CK3dEntity *obj) const {
    if (!obj) return VxVector(0, 0, 0);

    VxVector pos;
    obj->GetPosition(&pos);
    return pos;
}

VxQuaternion GameInterface::GetRotation(CK3dEntity *obj) const {
    if (!obj) return VxQuaternion(0, 0, 0, 1);

    VxQuaternion rot;
    obj->GetQuaternion(&rot);
    return rot;
}

VxVector GameInterface::GetVelocity(CK3dEntity *obj) const {
    if (!obj || !m_IpionManager) return VxVector(0, 0, 0);

    PhysicsObject *physObj = m_IpionManager->GetPhysicsObject(obj);
    if (!physObj) return VxVector(0, 0, 0);

    VxVector vel;
    physObj->GetVelocity(&vel, nullptr);
    return vel;
}

VxVector GameInterface::GetAngularVelocity(CK3dEntity *obj) const {
    if (!obj || !m_IpionManager) return VxVector(0, 0, 0);

    PhysicsObject *physObj = m_IpionManager->GetPhysicsObject(obj);
    if (!physObj) return VxVector(0, 0, 0);

    VxVector angVel;
    physObj->GetVelocity(nullptr, &angVel);
    return angVel;
}

bool GameInterface::IsSleeping(CK3dEntity *obj) const {
    if (!obj || !m_IpionManager) return false;

    // This requires access to the underlying physics object.
    // The IPhysicsObject interface in BML might not expose this directly.
    // We may need to use the older `CKIpionManager::GetPhysicsObject` to access the concrete type.
    PhysicsObject *ipionObj = m_IpionManager->GetPhysicsObject(obj);
    if (!ipionObj) return false;

#if !defined(__APPLE__)
    if (PhysicsRuntimeBridgeActive()) {
        bool sleeping = false;
        return PhysicsRuntimeGetObjectState(
            ipionObj, nullptr, nullptr, nullptr, nullptr, &sleeping) && sleeping;
    }
#endif

    return ipionObj->m_RealObject->get_core()->movement_state >= IVP_MT_CALM;
}

XObjectArray GameInterface::GetFloors(CK3dEntity *ent, float zoom, float maxHeight) const {
    XObjectArray floors;

    const VxBbox &Bbox_obj = ent->GetBoundingBox();
    float inv_zoom = 1.0f / (zoom * (Bbox_obj.Max.x - Bbox_obj.Min.x));

    if (!m_CKContext) return floors;

    auto *FloorManager = (CKFloorManager *) m_CKContext->GetManagerByGuid(FLOOR_MANAGER_GUID);
    int floorAttribute = FloorManager->GetFloorAttribute();

    CKAttributeManager *attman = m_CKContext->GetAttributeManager();
    const XObjectPointerArray &floor_objects = attman->GetGlobalAttributeListPtr(floorAttribute);

    VxVector vPos, scale;

    int nbf_under = 0; // number of floors under

    int floor_count = floor_objects.Size();

    for (int n = 0; n < floor_count; ++n) {
        auto *floor = (CK3dEntity *) floor_objects[n];

        if (floor->IsVisible() && !floor->IsAllOutsideFrustrum()) {
            // ... and just the VISIBLE floors
            // rem: IsAllOutsideFrustrum() function will result in a 1 frame delayed appearance
            // if the floor was totally outside the viewing frustum during the previous rendering,
            // and becomes very visible now... (if this is the case, the IsAllOutsideFrustrum()
            // function can be removed ).

            bool under = false;

            const VxBbox &Bbox_floorWorld = floor->GetBoundingBox();

            // test the two world bbox on XZ plane
            if (Bbox_floorWorld.Min.x > Bbox_obj.Max.x ||
                Bbox_floorWorld.Min.z > Bbox_obj.Max.z ||
                Bbox_floorWorld.Max.x < Bbox_obj.Min.x ||
                Bbox_floorWorld.Max.z < Bbox_obj.Min.z)
                continue;

            // test the two bbox on height
            if (Bbox_floorWorld.Min.y > Bbox_obj.Max.y ||           // floor is above the object
                Bbox_obj.Min.y - Bbox_floorWorld.Max.y > maxHeight) // floor is too below
                continue;

            ent->GetPosition(&vPos, floor);
            const VxBbox &Bbox_floor = floor->GetBoundingBox(TRUE);

            floor->GetScale(&scale);

            float tmp_x = inv_zoom * scale.x;
            float tmp_z = inv_zoom * scale.z;

            if ((Bbox_floor.Max.x - vPos.x) * tmp_x >= -0.5f)
                if ((Bbox_floor.Min.x - vPos.x) * tmp_x <= 0.5f)
                    if ((Bbox_floor.Max.z - vPos.z) * tmp_z >= -0.5f)
                        if ((Bbox_floor.Min.z - vPos.z) * tmp_z <= 0.5f) {
                            // floors is UNDER if the UV of its Bbox
                            // are possibly in the texture (0-1)
                            under = true;
                        }

            if (under) {
                // limit to 50 floors
                if (nbf_under < 50) {
                    floors.PushBack(floor->GetID());
                    nbf_under++;
                }
            }
        }
    }

    return floors;
}

// ========================================
// Gameplay State Queries
// ========================================

bool GameInterface::IsIngame() const {
    return m_BML->IsIngame();
}

bool GameInterface::IsPaused() const {
    return m_BML->IsPaused();
}

bool GameInterface::IsPlaying() const {
    return m_BML->IsPlaying();
}

int GameInterface::GetCurrentLevel() const {
    int level = -1;
    if (m_CurrentLevel) {
        m_CurrentLevel->GetElementValue(0, 0, &level);
    }
    return level;
}

int GameInterface::GetCurrentSector() const {
    int sector = -1;
    if (m_CurrentSector) {
        m_CurrentSector->GetValue(&sector);
    }
    return sector;
}

int GameInterface::GetPoints() const {
    int points = 0;
    if (m_Energy) {
        m_Energy->GetElementValue(0, 0, &points);
    }
    return points;
}

bool GameInterface::SetPoints(int points) {
    if (!SetDataArrayValue(m_Energy, 0, 0, points)) {
        Log::Error("Failed to set points: Energy array not available or write failed.");
        return false;
    }

    return true;
}

int GameInterface::GetLifeCount() const {
    int life = 0;
    if (m_Energy) {
        m_Energy->GetElementValue(0, 1, &life);
    }
    return life;
}

bool GameInterface::SetLifeCount(int lives) {
    if (!SetDataArrayValue(m_Energy, 0, 1, lives)) {
        Log::Error("Failed to set life count: Energy array not available or write failed.");
        return false;
    }

    return true;
}

bool GameInterface::SetCurrentSector(int sector) {
    if (!SetParameterValue(m_CurrentSector, sector)) {
        Log::Error("Failed to set current sector: parameter not available.");
        return false;
    }

    return true;
}

float GameInterface::GetSRScore() const {
    if (m_SRScoreOverride.has_value()) {
        return *m_SRScoreOverride;
    }

    return m_BML->GetSRScore();
}

bool GameInterface::SetSRScore(float score) {
    m_SRScoreOverride = score;
    Log::Warn("SetSRScore: BML exposes SR score as read-only; using TAS-layer override for restored state.");
    return true;
}

int GameInterface::GetHSScore() const {
    if (m_HSScoreOverride.has_value()) {
        return *m_HSScoreOverride;
    }

    return m_BML->GetHSScore();
}

bool GameInterface::SetHSScore(int score) {
    m_HSScoreOverride = score;
    Log::Warn("SetHSScore: BML exposes HS score as read-only; using TAS-layer override for restored state.");
    return true;
}

// ========================================
// UI & Output
// ========================================

void GameInterface::SetUIMode(UIMode mode) {
    if (GetUIManager()) {
        GetUIManager()->SetMode(mode);
    }
}

void GameInterface::PrintMessage(const char *message) const {
    m_BML->SendIngameMessage(message);
}

void GameInterface::SkipRenderForTicks(size_t ticks) {
    m_Mod->SkipRenderingForTicks(ticks);
}

namespace {
    struct MenuScreen {
        const char *script;
        const char *name;
    };

    // Overlays first, so the screen the player actually sees wins.
    constexpr MenuScreen kMenuScreens[] = {
        {"Menu_YesNo", "yesno"},
        {"Menu_HighscoreEntry", "highscore_entry"},
        {"Menu_Pause", "pause"},
        {"Menu_Dead", "dead"},
        {"Menu_End", "end"},
        {"Menu_Score", "score"},
        {"Menu_Opt_Graphics", "options_graphics"},
        {"Menu_Opt_Keys", "options_keys"},
        {"Menu_Opt_Sound", "options_sound"},
        {"Menu_Options", "options"},
        {"Menu_Highscore", "highscore"},
        {"Menu_Credits", "credits"},
        {"Menu_Zone", "zone"},
        {"Menu_Start", "start"},
        {"Menu_Main", "main"},
    };
}

std::string GameInterface::GetActiveMenu() const {
    for (const auto &screen : kMenuScreens) {
        CKBehavior *script = m_BML->GetScriptByName(screen.script);
        if (script && script->IsActive()) {
            return screen.name;
        }
    }
    return {};
}

namespace {
    struct MenuButton {
        const char *screen;
        const char *button; // name scripts use
        const char *entity; // 2D entity the screen's TT PushButton2 targets
    };

    // Button names per screen, in the order the screens list them.
    constexpr MenuButton kMenuButtons[] = {
        {"main", "start", "M_Main_But_1"},
        {"main", "highscore", "M_Main_But_2"},
        {"main", "options", "M_Main_But_3"},
        {"main", "credits", "M_Main_But_4"},
        {"main", "exit", "M_Main_But_5"},
        {"start", "back", "M_Start_But_Back"},
        {"start", "zone", "M_Start_But_13"},
        {"pause", "restart", "M_Pause_But_1"},
        {"pause", "highscore", "M_Pause_But_2"},
        {"pause", "options", "M_Pause_But_3"},
        {"pause", "exit", "M_Pause_But_4"},
        {"pause", "back", "M_Pause_But_Back"},
        {"pause", "continue", "M_Pause_But_Back"},
        {"yesno", "yes", "M_YesNo_But_Yes"},
        {"yesno", "no", "M_YesNo_But_No"},
        {"yesno", "back", "M_YesNo_But_No"},
        {"dead", "restart", "M_Dead_But_1"},
        {"dead", "main", "M_Dead_But_2"},
        {"end", "restart", "M_End_But_1"},
        {"end", "highscore", "M_End_But_2"},
        {"end", "options", "M_End_But_3"},
        {"end", "main", "M_End_But_4"},
        {"end", "next", "M_End_But_5"},
        {"options", "graphics", "M_Options_But_1"},
        {"options", "keys", "M_Options_But_2"},
        {"options", "sound", "M_Options_But_3"},
        {"options", "back", "M_Options_But_Back"},
        {"highscore", "back", "M_Highscore_But_Back"},
        {"highscore", "next", "M_Highscore_But_Back"},
        {"highscore", "previous_table", "M_Highscore_But_1"},
        {"highscore", "next_table", "M_Highscore_But_2"},
        {"highscore_entry", "ok", "M_HighEntry_But_1"},
        {"credits", "back", "M_Credits_But_Back"},
    };

    const char *MenuScriptName(const std::string &screen) {
        for (const auto &entry : kMenuScreens) {
            if (screen == entry.name) {
                return entry.script;
            }
        }
        return nullptr;
    }

    std::string Lowered(std::string text) {
        for (char &c : text) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return text;
    }

    // Activates whatever the given output leads to, climbing out of graphs whose
    // output it is wired to, the way the behavioral engine would on the next frame.
    bool ActivateOutputTarget(CKBehavior *owner, CKBehaviorIO *output) {
        for (int depth = 0; owner && output && depth < 8; ++depth) {
            CKBehavior *graph = owner->GetParent();
            if (!graph) {
                return false;
            }
            CKBehaviorIO *next = nullptr;
            for (int i = 0; i < graph->GetSubBehaviorLinkCount(); ++i) {
                CKBehaviorLink *link = graph->GetSubBehaviorLink(i);
                if (link && link->GetInBehaviorIO() == output) {
                    next = link->GetOutBehaviorIO();
                    break;
                }
            }
            if (!next) {
                return false;
            }
            CKBehavior *target = next->GetOwner();
            if (!target) {
                return false;
            }
            for (int k = 0; k < target->GetInputCount(); ++k) {
                if (target->GetInput(k) == next) {
                    target->ActivateInput(k);
                    target->Activate();
                    return true;
                }
            }
            // The link ends on an output of the enclosing graph: keep following it outward.
            owner = target;
            output = next;
        }
        return false;
    }
}

std::vector<std::string> GameInterface::GetMenuButtons(const std::string &screen) const {
    std::vector<std::string> buttons;
    for (const auto &entry : kMenuButtons) {
        if (screen == entry.screen) {
            buttons.emplace_back(entry.button);
        }
    }
    if (screen == "start") {
        for (int level = LevelNames::kFirstLevel; level <= 12; ++level) {
            buttons.push_back("level_" + std::to_string(level));
        }
    } else if (screen == "zone") {
        buttons.emplace_back("level_13");
        buttons.emplace_back("back");
    } else if (screen == "options_graphics" || screen == "options_keys" || screen == "options_sound") {
        buttons.emplace_back("back");
    }
    return buttons;
}

bool GameInterface::ClickMenuButton(const std::string &buttonName, std::string &error) {
    const std::string screen = GetActiveMenu();
    if (screen.empty()) {
        error = "no menu is active";
        return false;
    }
    const std::string button = Lowered(buttonName);
    CKBehavior *script = m_BML->GetScriptByName(MenuScriptName(screen));
    if (!script) {
        error = "the '" + screen + "' menu script was not found";
        return false;
    }

    // Level buttons go through LoadLevel, which does what the button does.
    if (button.rfind("level_", 0) == 0 && (screen == "start" || screen == "zone")) {
        const auto level = LevelNames::Parse(button.substr(6));
        if (!level || (screen == "zone") != (*level == 13)) {
            error = "'" + buttonName + "' is not a button on the '" + screen + "' menu";
            return false;
        }
        return LoadLevel(*level, error);
    }

    // Screens whose "back" is their Exit graph (Esc on those screens does the same).
    if (button == "back" && (screen == "options_graphics" || screen == "options_keys" ||
                             screen == "options_sound" || screen == "zone")) {
        CKBehavior *exit = ScriptHelper::FindFirstBB(script, "Exit");
        if (!exit) {
            error = "the '" + screen + "' menu has no Exit graph";
            return false;
        }
        exit->ActivateInput(0);
        exit->Activate();
        return true;
    }

    const char *entityName = nullptr;
    for (const auto &entry : kMenuButtons) {
        if (screen == entry.screen && (button == entry.button || buttonName == entry.entity)) {
            entityName = entry.entity;
            break;
        }
    }
    if (!entityName) {
        error = "'" + buttonName + "' is not a button on the '" + screen + "' menu";
        return false;
    }

    CKBehavior *pushButton = nullptr;
    ScriptHelper::FindBB(script, [&](CKBehavior *beh) {
        CKBeObject *target = beh->GetTarget();
        if (target && target->GetName() && std::string(target->GetName()) == entityName) {
            pushButton = beh;
            return false;
        }
        return true;
    }, "TT PushButton2", true);
    if (!pushButton) {
        error = std::string("the '") + buttonName + "' button was not found";
        return false;
    }
    // Hidden, locked and removed buttons are switched off by their menu.
    if (!pushButton->IsActive()) {
        error = std::string("the '") + buttonName + "' button is not available right now";
        return false;
    }
    if (!ActivateOutputTarget(pushButton, pushButton->GetOutput(2))) {
        error = std::string("could not press '") + buttonName + "'";
        return false;
    }
    Log::Info("Pressed menu button '%s' on '%s'.", button.c_str(), screen.c_str());
    return true;
}

bool GameInterface::LoadLevel(int level, std::string &error) {
    if (IsIngame()) {
        error = "already in a level; exit to the menu first";
        return false;
    }

    // Leave the menu the same way its own level buttons do (see BML+ CustomMaps).
    const std::string menu = GetActiveMenu();
    CKBehavior *exitMenu = nullptr;
    if (menu == "start") {
        exitMenu = ScriptHelper::FindFirstBB(m_BML->GetScriptByName("Menu_Start"), "Exit");
    } else if (menu == "main") {
        exitMenu = ScriptHelper::FindFirstBB(m_BML->GetScriptByName("Menu_Main"), "Exit", false, 1, 0);
    } else if (menu == "zone") {
        // The Twilight Zone screen deactivates itself after its pick instead of using an Exit graph.
        exitMenu = m_BML->GetScriptByName("Menu_Zone");
    } else {
        error = menu.empty() ? "no menu is active" : "close the '" + menu + "' menu first";
        return false;
    }

    CKDataArray *currentLevel = m_CurrentLevel ? m_CurrentLevel : m_BML->GetArrayByName("CurrentLevel");
    CKLevel *scene = m_CKContext->GetCurrentLevel();
    CKGroup *allSound = m_BML->GetGroupByName("All_Sound");
    CKMessageManager *messages = m_CKContext->GetMessageManager();
    if (!exitMenu || !currentLevel || !scene || !allSound || !messages) {
        error = "the menu scene is incomplete";
        return false;
    }
    if (!currentLevel->SetElementValue(0, 0, &level)) {
        error = "could not set CurrentLevel";
        return false;
    }

    messages->SendMessageSingle(messages->AddMessageType((CKSTRING) "Load Level"), scene);
    messages->SendMessageSingle(messages->AddMessageType((CKSTRING) "Menu_Load"), allSound);
    if (CK2dEntity *blackScreen = m_BML->Get2dEntityByName("M_BlackScreen")) {
        blackScreen->Show(CKHIDE);
    }
    if (menu == "zone") {
        exitMenu->Activate(FALSE);
    } else {
        exitMenu->ActivateInput(0);
        exitMenu->Activate();
    }
    Log::Info("Loading level %d from the '%s' menu.", level, menu.c_str());
    return true;
}

bool GameInterface::ReadGameFlowMessage(int pin, CKMessageType &type, std::string &error) const {
    CKBehavior *handler = m_BML->GetScriptByName("Event_handler");
    CKBehavior *dispatch = handler
        ? ScriptHelper::FindFirstBB(handler, "Switch On Message", false, 2, 11, 11, 0)
        : nullptr;
    if (!dispatch) {
        error = "the game's Event_handler script was not found";
        return false;
    }
    // Read the message type the game itself waits for instead of hardcoding names.
    type = -1;
    if (dispatch->GetInputParameterValue(pin, &type) != CK_OK || type < 0) {
        error = "could not read the Event_handler message type";
        return false;
    }
    return true;
}

void GameInterface::ExitMenuScript(const char *scriptName) {
    CKBehavior *script = m_BML->GetScriptByName(scriptName);
    if (!script || !script->IsActive()) {
        return;
    }
    if (CKBehavior *exit = ScriptHelper::FindFirstBB(script, "Exit")) {
        exit->ActivateInput(0);
        exit->Activate();
    }
}

void GameInterface::DispatchGameFlowMessage(CKMessageType type, bool toBalls, bool ballsFirst) {
    CKMessageManager *messages = m_CKContext->GetMessageManager();
    CKLevel *scene = m_CKContext->GetCurrentLevel();
    CKGroup *allBalls = toBalls ? m_BML->GetGroupByName("All_Balls") : nullptr;
    if (!messages || !scene || (toBalls && !allBalls)) {
        Log::Error("Game event could not be delivered: the level scene is incomplete.");
        return;
    }
    const char *name = messages->GetMessageTypeName(type);
    Log::Info("Sending game event '%s'%s.", name ? name : "?", toBalls ? " to the level and all balls" : "");
    if (ballsFirst && allBalls) {
        messages->SendMessageSingle(type, allBalls);
    }
    messages->SendMessageSingle(type, scene);
    if (!ballsFirst && allBalls) {
        messages->SendMessageSingle(type, allBalls);
    }
}

bool GameInterface::SendGameFlowEvent(GameFlowEvent event, std::string &error) {
    if (!IsIngame()) {
        error = "not in a level";
        return false;
    }
    const bool completed = IsLevelCompleted();
    if (event == GameFlowEvent::NextLevel && !completed) {
        error = "the next level is only available after finishing this one";
        return false;
    }

    CKMessageType type = -1;
    if (!ReadGameFlowMessage(static_cast<int>(event), type, error)) {
        return false;
    }

    // Replay what the game's own buttons do (Menu.nmo):
    // - End screen (level finished): leave Menu_End, then message the level.
    // - Mid-level (pause menu): the level is paused first, which stops the
    //   gameplay scripts that push the ball; then the pause menu is left and
    //   the message goes to the level and every ball. Resetting while those
    //   scripts still run corrupts the game's heap.
    if (completed) {
        ExitMenuScript("Menu_End");
        DispatchGameFlowMessage(type, false, false);
        return true;
    }

    CKMessageType pauseType = -1;
    if (!ReadGameFlowMessage(static_cast<int>(GameFlowEvent::PauseLevel), pauseType, error)) {
        return false;
    }
    if (!m_BML->GetGroupByName("All_Balls")) {
        error = "the All_Balls group was not found";
        return false;
    }

    const bool ballsFirst = event == GameFlowEvent::ExitLevel;
    auto leaveMenuAndSend = [this, type, ballsFirst]() {
        ExitMenuScript("Menu_Pause");
        // The pause menu's YesNo step waits 2 frames before sending.
        m_BML->AddTimer(static_cast<CKDWORD>(2), [this, type, ballsFirst]() {
            DispatchGameFlowMessage(type, true, ballsFirst);
        });
    };

    CKBehavior *pauseMenu = m_BML->GetScriptByName("Menu_Pause");
    if (pauseMenu && pauseMenu->IsActive()) {
        leaveMenuAndSend();
    } else {
        if (CKMessageManager *messages = m_CKContext->GetMessageManager()) {
            Log::Info("Pausing the level before the game event.");
            messages->SendMessageSingle(pauseType, m_CKContext->GetCurrentLevel());
        }
        m_BML->AddTimer(static_cast<CKDWORD>(2), leaveMenuAndSend);
    }
    return true;
}

void GameInterface::OnCloseMenu() {
    CKBehavior *beh = m_BML->GetScriptByName("Menu_Start");
    if (beh) {
        m_CKContext->GetCurrentScene()->Activate(beh, true);
    }

    m_BML->AddTimerLoop(static_cast<CKDWORD>(1), [this] {
        if (m_InputManager->IsKeyDown(CKKEY_ESCAPE) || m_InputManager->IsKeyDown(CKKEY_RETURN))
            return true;
#ifndef __APPLE__
        // The retail CKInputManager has no blocking facade. On Windows that
        // state belongs to BML's InputHook; the native CK2 implementation also
        // exposes compatible helpers directly on CKInputManager.
        if (InputHook *input = m_BML->GetInputManager())
            input->Unblock(CK_INPUT_DEVICE_KEYBOARD);
#endif
        return false;
    });
}

// ========================================
// Utilities
// ========================================

void GameInterface::AddTimer(size_t tick, const std::function<void()> &callback) {
    m_BML->AddTimer(static_cast<CKDWORD>(tick), callback);
}
