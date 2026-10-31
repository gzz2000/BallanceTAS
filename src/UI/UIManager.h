#pragma once

#include <memory>

#include <BML/Bui.h>

// Forward declarations
class TASEngine;
class TASMenu;
class InGameOSD;
enum class OSDPanel;

/**
 * @enum UIMode
 * @brief Represents the current high-level state of the UI for display purposes.
 */
enum class UIMode {
    Idle      = 0,
    Playing   = 1,
    Paused    = 2,
    Recording = 3
};

/**
 * @class UIManager
 * @brief Manages all user interface components for the TAS framework.
 *
 * This class provides a centralized system for managing TAS-related UI components,
 * handling their lifecycle, state synchronization, and rendering coordination.
 * It serves as the main interface between the TAS engine and the visual components.
 * UIManager delegates recording and replay control to TASEngine for consistency.
 */
class UIManager {
public:
    explicit UIManager(TASEngine *engine);
    ~UIManager();

    // UIManager is not copyable or movable
    UIManager(const UIManager &) = delete;
    UIManager &operator=(const UIManager &) = delete;

    /**
     * @brief Initializes all UI components and subsystems.
     * @return True on success, false on failure.
     */
    bool Initialize();

    /**
     * @brief Shuts down and cleans up all UI resources.
     */
    void Shutdown();

    /**
     * @brief Processes UI logic and input handling.
     * Should be called every frame before rendering.
     */
    void Process();

    /**
     * @brief Renders all active UI components.
     * Should be called during the game's render phase.
     */
    void Render();

    // --- TAS Menu Control ---

    /**
     * @brief Opens the main TAS Menu.
     */
    void OpenTASMenu();

    /**
     * @brief Closes the TAS Menu if open.
     */
    void CloseTASMenu();

    /**
     * @brief Toggles the visibility of the main TAS Menu.
     */
    void ToggleTASMenu();

    /**
     * @brief Checks if the TAS menu is currently open.
     * @return True if any TAS menu page is visible.
     */
    bool IsTASMenuOpen() const;

    // --- Recording Control ---

    /**
     * @brief Starts recording a new TAS.
     * @return True if recording started successfully.
     */
    bool StartRecording();

    /**
     * @brief Stops recording (auto-generates script).
     * @return True if recording was stopped.
     */
    bool StopRecording();

    /**
     * @brief Toggles recording state.
     */
    void ToggleRecording();

    /**
     * @brief Checks if currently recording.
     * @return True if recording is active or pending.
     */
    bool IsRecording() const;

    // --- Replay Control ---

    /**
     * @brief Starts replaying the current TAS project.
     * @return True if replay started successfully.
     */
    bool StartReplay();

    /**
     * @brief Stops replay.
     * @return True if replay was stopped.
     */
    bool StopReplay();

    /**
     * @brief Toggles replay state.
     */
    void ToggleReplay();

    /**
     * @brief Checks if currently replaying.
     * @return True if replay is active or pending.
     */
    bool IsReplaying() const;

    // === Translation Control ===

    /**
     * @brief Starts translating the current record project to script format.
     * @return True if translation started successfully.
     */
    bool StartTranslation();

    /**
     * @brief Stops translation.
     * @return True if translation was stopped.
     */
    bool StopTranslation();

    /**
     * @brief Toggles translation state.
     */
    void ToggleTranslation();

    /**
     * @brief Checks if currently translating.
     * @return True if translation is active or pending.
     */
    bool IsTranslating() const;

    // --- OSD Control ---

    /**
     * @brief Sets the visibility of the In-Game OSD.
     * @param visible True to show, false to hide.
     */
    void SetOSDVisible(bool visible);

    /**
     * @brief Toggles the In-Game OSD visibility.
     */
    void ToggleOSD();

    /**
     * @brief Checks if the OSD is currently visible.
     * @return True if OSD is visible.
     */
    bool IsOSDVisible() const;

    /**
     * @brief Toggles a specific OSD panel.
     * @param panel The panel to toggle.
     */
    void ToggleOSDPanel(OSDPanel panel);

    // --- State Management ---

    /**
     * @brief Updates the current UI mode across all components.
     * @param mode The new UIMode (Playing, Recording, etc.).
     */
    void SetMode(UIMode mode);

    /**
     * @brief Gets the current UI mode.
     * @return The current UIMode.
     */
    UIMode GetMode() const { return m_CurrentMode; }

    // --- Component Access ---

    /**
     * @brief Gets the TAS menu instance.
     * @return Pointer to the TAS menu, or nullptr if not initialized.
     */
    TASMenu *GetTASMenu() const {
#if !TAS_ENABLE_LEGACY_MENU
        return nullptr;
#else
        return m_TASMenu.get();
#endif
    }

    /**
     * @brief Gets the In-Game OSD instance.
     * @return Pointer to the OSD, or nullptr if not initialized.
     */
    InGameOSD *GetOSD() const { return m_InGameOSD.get(); }

    // --- Configuration ---

    /**
     * @brief Sets the hotkey for stopping TAS playback or recording.
     * @param key The key code (default: CKKEY_F3).
     */
    void SetStopHotkey(CKKEYBOARD key) { m_StopHotkey = Bui::CKKeyToImGuiKey(key); }
    const char *GetStopHotkeyName() const { return ImGui::GetKeyName(m_StopHotkey); }

    /**
     * @brief Sets the hotkey for toggling the OSD.
     * @param key The key code (default: CKKEY_F11).
     */
    void SetOSDHotkey(CKKEYBOARD key) { m_OSDHotkey = Bui::CKKeyToImGuiKey(key); }

private:
    // --- Internal Methods ---
    void UpdateHotkeys();

    // --- Core References ---
    TASEngine *m_Engine;

    // --- UI Components ---
#if TAS_ENABLE_LEGACY_MENU
    std::unique_ptr<TASMenu> m_TASMenu;
#endif
    std::unique_ptr<InGameOSD> m_InGameOSD;

    // --- State ---
    bool m_Initialized = false;
    bool m_OSDVisible = true;
    UIMode m_CurrentMode = UIMode::Idle;

    // --- Configuration ---
    ImGuiKey m_StopHotkey = ImGuiKey_F3;
    ImGuiKey m_OSDHotkey = ImGuiKey_F11;
    // OSD panel toggles
    ImGuiKey m_StatusPanelHotkey = ImGuiKey_F5;
    ImGuiKey m_BallPanelHotkey = ImGuiKey_F6;
    ImGuiKey m_InputPanelHotkey = ImGuiKey_F4;
    ImGuiKey m_ValidationPanelHotkey = ImGuiKey_F7;
    ImGuiKey m_WatchesPanelHotkey = ImGuiKey_F8;
};
