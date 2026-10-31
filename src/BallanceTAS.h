#pragma once

#include <memory>

#include <BML/BMLAll.h>

#include "ConfigService.h"
#include "HookManager.h"
#include "LegacyPreloadSession.h"

// Forward-declare the core engine to avoid including its full header here.
// This reduces compile times and keeps dependencies clean.
class EventBus;
class GameInterface;
class HookManager;
class TASEngine;
class UIManager;
class BMLLogSink;
class PlayerHostBridge;

/**
 * @class BallanceTAS
 * @brief The main BML Mod class for the BallanceTAS framework.
 *
 * This class serves as the entry point for the BML loader. It is responsible for:
 * 1. Initializing and shutting down the core TASEngine and UIManager.
 * 2. Handling BML configuration properties.
 * 3. Receiving game lifecycle events from BML (e.g., OnStartLevel) and
 *    forwarding them to the appropriate systems within the framework.
 * 4. Driving the core engine's Tick() and UI Render() methods every frame.
 * 5. Coordinating between TAS logic and UI systems.
 */
class BallanceTAS : public IMod {
public:
    explicit BallanceTAS(IBML *bml);
    ~BallanceTAS() override;

    //================================================================
    // IMod Interface Implementation
    //================================================================

    const char *GetID() override { return "BallanceTAS"; }
    const char *GetVersion() override { return "2.0.0-beta.1"; }
    const char *GetName() override { return "Ballance TAS"; }
    const char *GetAuthor() override { return "Ballance TAS Community"; }

    const char *GetDescription() override {
        return "A tool-assisted speedrun framework for Ballance.";
    }

    DECLARE_BML_VERSION;

    // Core lifecycle methods
    void OnLoad() override;
    void OnUnload() override;

    // Per-frame processing
    void OnProcess() override;
    void OnRender(CK_RENDER_FLAGS flags) override;

    // Configuration handling
    void OnModifyConfig(const char *category, const char *key, IProperty *prop) override;

    void OnLoadObject(const char *filename, CKBOOL isMap, const char *masterName, CK_CLASSID filterClass,
                      CKBOOL addToScene, CKBOOL reuseMeshes, CKBOOL reuseMaterials, CKBOOL dynamic,
                      XObjectArray *objArray, CKObject *masterObj) override;
    void OnLoadScript(const char *filename, CKBehavior *script) override;
    void OnPhysicalize(CK3dEntity *target, CKBOOL fixed, float friction, float elasticity, float mass,
                       const char *collGroup, CKBOOL startFrozen, CKBOOL enableColl, CKBOOL calcMassCenter,
                       float linearDamp, float rotDamp, const char *collSurface, VxVector massCenter,
                       int convexCnt, CKMesh **convexMesh, int ballCnt, VxVector *ballCenter, float *ballRadius,
                       int concaveCnt, CKMesh **concaveMesh) override;
    void OnUnphysicalize(CK3dEntity *target) override;
    std::string GetLegacyPreloadError() const {
#if defined(__APPLE__)
        return m_LegacyPreload.Error();
#else
        return {};
#endif
    }

    //================================================================
    // Game Event Forwarding
    //================================================================

    void OnPreStartMenu() override;
    void OnPostStartMenu() override;

    void OnExitGame() override;

    void OnPreLoadLevel() override;
    void OnPostLoadLevel() override;

    void OnStartLevel() override;

    void OnPreResetLevel() override;
    void OnPostResetLevel() override;

    void OnPauseLevel() override;
    void OnUnpauseLevel() override;

    void OnPreExitLevel() override;
    void OnPostExitLevel() override;

    void OnPreNextLevel() override;
    void OnPostNextLevel() override;

    void OnDead() override;

    void OnPreEndLevel() override;
    void OnPostEndLevel() override;

    void OnCounterActive() override;
    void OnCounterInactive() override;

    void OnBallNavActive() override;
    void OnBallNavInactive() override;

    void OnCamNavActive() override;
    void OnCamNavInactive() override;

    void OnBallOff() override;

    void OnPreCheckpointReached() override;
    void OnPostCheckpointReached() override;

    void OnLevelFinish() override;

    void OnGameOver() override;

    void OnExtraPoint() override;

    void OnPreSubLife() override;
    void OnPostSubLife() override;

    void OnPreLifeUp() override;
    void OnPostLifeUp() override;

    //================================================================
    // Public Accessors
    //================================================================

    IBML *GetBML() const { return m_BML; }
    ILogger *GetLogger() const { return m_Logger; }
    InputHook *GetInputManager() const { return m_InputManager; }
    UIManager *GetUIManager() const { return m_UIManager.get(); }
    TASEngine *GetEngine() const { return m_Engine.get(); }
    EventBus *GetEventBus() const { return m_EventBus.get(); }
    HookManager *GetHookManager() const { return m_HookManager.get(); }
    ConfigService &GetConfigService() { return m_ConfigService; }

    //================================================================
    // UI Coordination Methods
    //================================================================

    /**
     * @brief Sets OSD visibility (called via config changes).
     * @param visible Whether the OSD should be visible.
     */
    void SetOSDVisible(bool visible);

    /**
     * @brief Updates OSD panel configuration based on config changes.
     */
    void UpdateOSDPanelConfig();

    /**
     * @brief Skips rendering for a specified number of ticks.
     * @param ticks The number of ticks to skip rendering for.
     */
    void SkipRenderingForTicks(size_t ticks);

private:
    /**
     * @brief Initializes the game hooks for TAS functionality.
     * Called during framework initialization.
     * @return True if hooks were initialized successfully.
     */
    bool InitializeGameHooks();

    /**
     * @brief Disables the game hooks for TAS functionality.
     * Called during framework shutdown.
     */
    void DisableGameHooks();
    void ApplyIdleScriptInput(CKInputManager *inputManager);

    /**
     * @brief Initializes the TASEngine and UIManager and all their subsystems.
     * This is called from OnLoad or when the mod is enabled via config.
     */
    bool Initialize();

    /**
     * @brief Shuts down and cleans up the TASEngine and UIManager.
     * This is called from OnUnload or when the mod is disabled.
     */
    void Shutdown();

    void OnMenuStart();

    // Picks up projects added, removed or edited while the game runs.
    void PollProjects();
    // TAS.SkipRenderUntilFrame: no rendering until playback reaches that frame.
    void SkipRenderingToConfiguredFrame();

    std::unique_ptr<GameInterface> m_GameInterface;

    // Event bus for decoupled communication between components.
    std::unique_ptr<EventBus> m_EventBus;

    // Hook manager owns the shared callback dispatchers for the mod lifetime.
    std::unique_ptr<HookManager> m_HookManager;

    // The single, top-level instance of the TAS framework's core engine.
    std::unique_ptr<TASEngine> m_Engine;

    // Optional same-process bridge to the dedicated TAS Player host.
    std::unique_ptr<PlayerHostBridge> m_PlayerHostBridge;

    // UI Manager for all TAS-related user interface components.
    std::unique_ptr<UIManager> m_UIManager;

    bool m_Initialized = false;
    LegacyPreloadSession m_LegacyPreload;
    bool m_GameHooksEnabled = false;
    // Whether the game's ball and camera control are on (BallNav / CamNav messages).
    bool m_BallNavActive = false;
    bool m_CamNavActive = false;

    // Applies script keys outside TAS playback, so global scripts can drive menus.
    ScopedCallback m_IdleScriptInputGuard;
    size_t m_SkipRenderingCount = 0;
    size_t m_ProjectPollFrames = 0;
    size_t m_ProjectChoicesGeneration = 0;

    ILogger *m_Logger = nullptr;
    InputHook *m_InputManager = nullptr;

    // Owns the BML->ILogSink adapter so its lifetime exceeds Log::Shutdown
    std::unique_ptr<BMLLogSink> m_LogSink;

    CK2dEntity *m_Level01 = nullptr;
    CKBehavior *m_ExitStart = nullptr;
    CKBehavior *m_ExitMain = nullptr;

    // Owns all BML IProperty* config registrations.
    ConfigService m_ConfigService;
};
