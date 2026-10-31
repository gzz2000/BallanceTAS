#pragma once

#include <CKAll.h>
#include <string>
#include <vector>
#include <functional>
#include <optional>

#include <BML/InputHook.h>

#include "physics_RT.h"
#include "PhysicsDeterminism.h"
#include "IGameControl.h"
#include "IGameQuery.h"
#include "IInputAccess.h"
#include "IPhysicsProvider.h"
#include "IObjectProvider.h"
#include "IGameStateProvider.h"
#include "ITimeProvider.h"

// Forward declarations — full headers deferred to GameInterface.cpp
enum class UIMode;

// Forward declarations
class BallanceTAS;
class CKIpionManager;
class UIManager;
class IBML;

/**
 * @class GameInterface
 * @brief Concrete implementation of all game-facing ISP interfaces.
 *
 * Implements IPhysicsProvider, IObjectProvider, IGameStateProvider,
 * and ITimeProvider. Components should depend
 * on the narrow interface they need rather than this full class.
 */
class GameInterface : public IPhysicsProvider,
                      public IObjectProvider,
                      public IGameQuery,
                      public IGameControl,
                      public IInputAccess,
                      public IGameStateProvider,
                      public ITimeProvider {
public:
    // ========================================
    // Construction & Destruction
    // ========================================
    explicit GameInterface(BallanceTAS *mod);

    // GameInterface is not copyable or movable
    GameInterface(const GameInterface &) = delete;
    GameInterface &operator=(const GameInterface &) = delete;

    // ========================================
    // Core & Manager Access
    // ========================================
    CKContext *GetCKContext() const override { return m_CKContext; }
    CKRenderContext *GetRenderContext() const override { return m_RenderContext; }
    CKTimeManager *GetTimeManager() const override { return m_TimeManager; }
    CKInputManager *GetInputManager() const override { return m_InputManager; }
    CKIpionManager *GetIpionManager() const { return m_IpionManager; }
    UIManager *GetUIManager() const;
    std::string GetLegacyPreloadError() const override;

    // ========================================
    // Initialization & Configuration
    // ========================================
    void AcquireGameplayInfo();
    void AcquireKeyBindings();

    void SetMapName(const std::string &name) { m_MapName = name; }
    const std::string &GetMapName() const { return m_MapName; }

    // ========================================
    // Physics & Time Management
    // ========================================
    void ResetPhysicsTime() override;
    void ResetPhysicsTime(float deltaTimeMs);
    void SetHostPhysicsTimeScale(float scale);
    /// Physics state a level load inherits from earlier levels (see PhysicsLevelBaseline).
    PhysicsDeterminism::PhysicsLevelBaseline CapturePhysicsLevelBaseline() const;
    /**
     * @brief Puts objects of equal priority in a fixed execution order (by object ID).
     *
     * The behavior manager runs objects by priority, and equal-priority objects in the order
     * they happen to sit in its list. That order changes with whatever the previous level
     * left behind, which makes Levelinit_build pick up its "Loaded" messages one frame
     * earlier or later, so a level load alternates between e.g. 101 and 102 frames.
     */
    void NormalizeBehaviorOrder();

    /**
     * @brief Ball and camera control a level left early would carry into the next level.
     *
     * The game turns them off when the ball falls or the level ends, but not when a level is
     * left early. Left on, Gameplay_Ingame's ball keys run before the next level hands the
     * ball over, and Sound_Manager's hit-sound collision listeners (which flip impact normals)
     * stay set. TurnOffNavigation() sends the game's own deactivate messages at the exit, which
     * reach the sound manager in time; StopLeftoverBallNavigation() turns the ball keys off at
     * the next load. A clean level load finds both off already.
     */
    void TurnOffNavigation(bool ballNav, bool camNav);
    void StopLeftoverBallNavigation();

    void RestorePhysicsLevelBaseline(const PhysicsDeterminism::PhysicsLevelBaseline &baseline);
    void SetPhysicsTimeFactor(float factor = 1.0f) override;

    // ========================================
    // Input Management
    // ========================================
    CKKEYBOARD RemapKey(CKKEYBOARD key) const;

    // ========================================
    // Object Access
    // ========================================

    /**
     * @brief Gets the CK3dEntity for the currently controlled ball.
     * @return A pointer to the ball's entity, or nullptr if not found.
     */
    CK3dEntity *GetActiveBall() const override;

    /**
     * @brief Sets the active ball parameter.
     * @param param The CKParameter that holds the active ball entity.
     */
    void SetActiveBall(CKParameter *param) override;

    /**
     * @brief Gets the active camera in the scene.
     * @return A pointer to the active CKCamera, or nullptr if not found.
     */
    CKCamera *GetActiveCamera() const override;

    /**
     * @brief Gets a game object by its name.
     * @param name The name of the CK3dEntity.
     * @return A pointer to the entity, or nullptr if not found.
     */
    CK3dEntity *GetObjectByName(const std::string &name) const override;

    /**
     * @brief Gets a game object by its id.
     * @param id The id of the CK3dEntity.
     * @return A pointer to the entity, or nullptr if not found.
     */
    CK3dEntity *GetObjectByID(int id) const override;

    /**
     * @brief Gets the PhysicsObject for a given CK3dEntity.
     * @param entity A pointer to the CK3dEntity.
     * @return A pointer to the PhysicsObject, or nullptr if not found.
     */
    PhysicsObject *GetPhysicsObject(CK3dEntity *entity) const override;

    // ========================================
    // Object Property Queries
    // ========================================

    /**
     * @brief Gets the world position of a game entity.
     * @param obj A pointer to the CK3dEntity.
     * @return The entity's position as a VxVector. Returns a zero vector if obj is null.
     */
    VxVector GetPosition(CK3dEntity *obj) const override;

    /**
     * @brief Gets the world rotation of a game entity.
     * @param obj A pointer to the CK3dEntity.
     * @return The entity's rotation as a VxQuaternion. Returns an identity quaternion if obj is null.
     */
    VxQuaternion GetRotation(CK3dEntity *obj) const override;

    /**
     * @brief Gets the world velocity of a game entity.
     * @param obj A pointer to the CK3dEntity.
     * @return The entity's linear velocity as a VxVector. Returns a zero vector if obj is null.
     */
    VxVector GetVelocity(CK3dEntity *obj) const override;

    /**
     * @brief Gets the angular velocity of a game entity.
     * @param obj A pointer to the CK3dEntity.
     * @return The entity's angular velocity. Returns a zero vector if obj is null.
     */
    VxVector GetAngularVelocity(CK3dEntity *obj) const override;

    /**
     * @brief Checks if a physics object is in a sleeping state.
     * @param obj A pointer to the CK3dEntity.
     * @return True if the object is sleeping, false otherwise.
     */
    bool IsSleeping(CK3dEntity *obj) const override;

    /**
     * @brief Gets the floors under a game entity.
     * @param ent The entity to check.
     * @param zoom The zoom factor for floor detection.
     * @param maxHeight The maximum height to check for floors.
     * @return An array of floor object IDs.
     */
    XObjectArray GetFloors(CK3dEntity *ent, float zoom = 2.0f, float maxHeight = 100.0f) const override;

    // ========================================
    // Gameplay State Queries
    // ========================================
    bool IsIngame() const override;
    bool IsPaused() const override;
    bool IsPlaying() const override;

    int GetCurrentLevel() const override;
    int GetCurrentSector() const override;

    int GetPoints() const override;
    bool SetPoints(int points) override;
    int GetLifeCount() const override;
    bool SetLifeCount(int lives) override;

    bool SetCurrentSector(int sector) override;

    float GetSRScore() const override;
    bool SetSRScore(float score) override;
    int GetHSScore() const override;
    bool SetHSScore(int score) override;

    // ========================================
    // UI & Output
    // ========================================
    void SetUIMode(UIMode mode);
    void PrintMessage(const char *message) const;
    void SkipRenderForTicks(size_t ticks);
    void OnCloseMenu();

    // ========================================
    // Game Flow
    // ========================================
    // These drive the game's own scripts the same way its menu buttons do.

    /// Pins of the Switch On Message in base.cmo's Event_handler script.
    enum class GameFlowEvent { ResetLevel = 4, PauseLevel = 5, ExitLevel = 7, NextLevel = 8 };

    /// Load built-in level 1-13 from the main menu or level selection.
    bool LoadLevel(int level, std::string &error);
    /// Restart, exit or advance the level the way the pause/end menus do. Asynchronous:
    /// returns once the sequence is scheduled; the level events report completion.
    bool SendGameFlowEvent(GameFlowEvent event, std::string &error);
    /// Topmost active menu screen ("main", "start", "pause", ...); empty when none is active.
    std::string GetActiveMenu() const;
    /// Button names click_button accepts on the given screen.
    std::vector<std::string> GetMenuButtons(const std::string &screen) const;
    /// Presses a button of the active menu by running the branch its own push button runs.
    bool ClickMenuButton(const std::string &button, std::string &error);

    bool IsLevelCompleted() const { return m_LevelCompleted; }
    void SetLevelCompleted(bool completed) { m_LevelCompleted = completed; }

    // ========================================
    // Utilities
    // ========================================
    void AddTimer(size_t tick, const std::function<void()> &callback);

private:
    bool ReadGameFlowMessage(int pin, CKMessageType &type, std::string &error) const;
    void ExitMenuScript(const char *scriptName);
    void DispatchGameFlowMessage(CKMessageType type, bool toBalls, bool ballsFirst);

    // ========================================
    // Core Managers
    // ========================================
    BallanceTAS *m_Mod = nullptr;
    IBML *m_BML = nullptr;

    CKContext *m_CKContext = nullptr;
    CKRenderContext *m_RenderContext = nullptr;
    CKTimeManager *m_TimeManager = nullptr;
    CKInputManager *m_InputManager = nullptr;
    CKIpionManager *m_IpionManager = nullptr;

    // ========================================
    // Game Data
    // ========================================
    std::string m_MapName;
    bool m_LevelCompleted = false;

    // ========================================
    // Gameplay Data Arrays
    // ========================================
    CKDataArray *m_Keyboard = nullptr;
    CKDataArray *m_CurrentLevel = nullptr;
    CKDataArray *m_Energy = nullptr;
    CKDataArray *m_CheckPoints = nullptr;
    CKDataArray *m_IngameParam = nullptr;

    // ========================================
    // Gameplay Parameters
    // ========================================
    CKParameter *m_CurrentSector = nullptr;
    CKParameter *m_ActiveBallParam = nullptr;

    // ========================================
    // TAS-layer score overrides
    // ========================================
    std::optional<float> m_SRScoreOverride;
    std::optional<int> m_HSScoreOverride;

    // ========================================
    // UI Data
    // ========================================
    CK2dEntity *m_Level01 = nullptr;
    CKBehavior *m_ExitStart = nullptr;
    CKBehavior *m_ExitMain = nullptr;

};
