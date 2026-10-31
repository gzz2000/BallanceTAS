#pragma once

#include <BML/BMLAll.h>

#include <string>
#include <vector>

class EventBus;

/// OSD panel configuration snapshot, read by UIManager/InGameOSD.
struct OSDConfig {
    bool showStatus = true;
    bool showBall = true;
    bool showInput = true;
    bool showValidation = true;
    bool showWatches = true;
    float positionX = 0.02f;
    float positionY = 0.02f;
    float opacity = 0.9f;
    float scale = 1.0f;
};

/// Startup script configuration snapshot.
struct StartupConfig {
    bool enabled = true;
    std::string project;
    std::string level; // "" or "Level_01" ... "Level_13"
    bool autoLoad = true;
};

/// Owns all BML IProperty* config registrations and provides typed accessors.
///
/// Publishes ConfigChangedEvent on the EventBus when any property is modified.
/// Created during OnLoad (before Initialize) since BML config registration
/// must happen early.
class ConfigService {
public:
    /// Register all config properties with BML. Call in OnLoad().
    void RegisterProperties(IConfig *config);

    /// Wire the EventBus for change notifications. Call during Initialize().
    void SetEventBus(EventBus *bus) { m_EventBus = bus; }

    /// Called from BallanceTAS::OnModifyConfig to dispatch typed change events.
    void OnPropertyChanged(const char *category, const char *key, IProperty *prop);

    // --- TAS Settings ---
    bool IsEnabled() const         { return m_Enabled && m_Enabled->GetBoolean(); }
    bool IsValidation() const      { return m_Validation && m_Validation->GetBoolean(); }
    bool IsAutoRestart() const     { return m_AutoRestart && m_AutoRestart->GetBoolean(); }
    bool PreserveLevelHistory() const { return m_PreserveLevelHistory && m_PreserveLevelHistory->GetBoolean(); }
    bool IsStopOnFinish() const    { return m_StopOnFinish && m_StopOnFinish->GetBoolean(); }
    /// Playback frame up to which rendering is skipped; 0 when off.
    int GetSkipRenderUntilFrame() const {
        const int frame = m_SkipRenderUntilFrame ? m_SkipRenderUntilFrame->GetInteger() : 0;
        return frame > 0 ? frame : 0;
    }
    CKKEYBOARD GetStopKey() const  { return m_StopKey ? m_StopKey->GetKey() : CKKEY_F3; }

    // --- Recording ---
    int GetMaxFrames() const { return m_RecordingMaxFrames ? m_RecordingMaxFrames->GetInteger() : 1000000; }

    // --- Startup Script ---
    StartupConfig GetStartupConfig() const;

    /// Offers these project names (plus "" for automatic selection) as the choices of
    /// Startup.Project. Call once the projects have been scanned.
    void SetStartupProjectChoices(const std::vector<std::string> &projectNames);

    // --- OSD ---
    bool IsOSDVisible() const { return m_ShowOSD && m_ShowOSD->GetBoolean(); }
    OSDConfig GetOSDConfig() const;

private:
    EventBus *m_EventBus = nullptr;

    // TAS
    IProperty *m_Enabled = nullptr;
    IProperty *m_Validation = nullptr;
    IProperty *m_AutoRestart = nullptr;
    IProperty *m_PreserveLevelHistory = nullptr;
    IProperty *m_StopOnFinish = nullptr;
    IProperty *m_SkipRenderUntilFrame = nullptr;
    IProperty *m_StopKey = nullptr;

    // Recording
    IProperty *m_RecordingMaxFrames = nullptr;

    // Startup Script
    IProperty *m_StartupScriptEnabled = nullptr;
    IProperty *m_StartupScriptProject = nullptr;
    IProperty *m_StartupLevel = nullptr;
    IProperty *m_AutoLoadStartupScript = nullptr;

    // OSD
    IProperty *m_ShowOSD = nullptr;
    IProperty *m_ShowOSDStatus = nullptr;
    IProperty *m_ShowOSDBall = nullptr;
    IProperty *m_ShowOSDInput = nullptr;
    IProperty *m_ShowOSDValidation = nullptr;
    IProperty *m_ShowOSDWatches = nullptr;
    IProperty *m_OSDPositionX = nullptr;
    IProperty *m_OSDPositionY = nullptr;
    IProperty *m_OSDOpacity = nullptr;
    IProperty *m_OSDScale = nullptr;
};
