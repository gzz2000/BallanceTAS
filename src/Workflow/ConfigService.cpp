#include "ConfigService.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <cstring>
#endif

#include <algorithm>

#include "EventBus.h"
#include "GameEvents.h"
#include "LevelNames.h"

// Shows a string property as radio buttons in the BML+ mod menu. The exports are looked
// up at run time: loaders older than the choice editor keep showing a text field.
static void SetChoiceEditor(IProperty *property, const std::vector<std::string> &choices) {
    using SetChoicesFn = int(__cdecl *)(IProperty *, const char *const[], size_t);
    using SetEditorFn = int(__cdecl *)(IProperty *, int);
    constexpr int kChoiceEditor = 2; // BML_CONFIG_EDITOR_CHOICE

    if (!property) {
        return;
    }
#ifdef _WIN32
    HMODULE loader = ::GetModuleHandleA("BMLPlus.dll");
    if (!loader) return;
    auto setChoices = reinterpret_cast<SetChoicesFn>(::GetProcAddress(loader, "BML_SetConfigPropertyChoices"));
    auto setEditor = reinterpret_cast<SetEditorFn>(::GetProcAddress(loader, "BML_SetConfigPropertyEditor"));
#else
    SetChoicesFn setChoices = nullptr;
    SetEditorFn setEditor = nullptr;
    for (uint32_t i = 0; i < _dyld_image_count() && (!setChoices || !setEditor); ++i) {
        const char *image = _dyld_get_image_name(i);
        if (!image || !std::strstr(image, "libBMLPlus.dylib")) continue;
        if (void *loader = dlopen(image, RTLD_LAZY | RTLD_NOLOAD)) {
            setChoices = reinterpret_cast<SetChoicesFn>(dlsym(loader, "BML_SetConfigPropertyChoices"));
            setEditor = reinterpret_cast<SetEditorFn>(dlsym(loader, "BML_SetConfigPropertyEditor"));
            dlclose(loader);
        }
    }
#endif
    if (!setChoices || !setEditor) {
        return;
    }

    std::vector<const char *> values;
    values.reserve(choices.size());
    for (const std::string &choice : choices) {
        values.push_back(choice.c_str());
    }
    if (setChoices(property, values.data(), values.size())) {
        setEditor(property, kChoiceEditor);
    }
}

void ConfigService::RegisterProperties(IConfig *config) {
    // --- TAS ---
    m_Enabled = config->GetProperty("TAS", "Enable");
    m_Enabled->SetComment("Enables TAS features.");
    m_Enabled->SetDefaultBoolean(true);

    m_Validation = config->GetProperty("TAS", "Validation");
    m_Validation->SetComment(
        "Enables validation recording for script playback. "
        "Records actual input during playback for verification.");
    m_Validation->SetDefaultBoolean(false);

    m_AutoRestart = config->GetProperty("TAS", "AutoRestart");
    m_AutoRestart->SetComment("Keep a record or level script armed after its playback ends, however it ends, "
                              "so it plays again the next time a level loads.");
    m_AutoRestart->SetDefaultBoolean(true);

    m_PreserveLevelHistory = config->GetProperty("TAS", "PreserveLevelHistory");
    m_PreserveLevelHistory->SetComment(
        "On the next TAS level load, keep the physics state left by earlier levels. "
        "Use for legacy records requiring a second playback in the same game process. "
        "Off restores the first load's physics baseline for repeatable practice. "
        "This does not change the clock-reset version or recreate missing recording environments.");
    m_PreserveLevelHistory->SetDefaultBoolean(false);

    m_SkipRenderUntilFrame = config->GetProperty("TAS", "SkipRenderUntilFrame");
    m_SkipRenderUntilFrame->SetComment("During playback, skip rendering until this playback frame to get there "
                                       "faster (0 renders every frame).");
    m_SkipRenderUntilFrame->SetDefaultInteger(0);

    m_StopOnFinish = config->GetProperty("TAS", "StopOnFinish");
    m_StopOnFinish->SetComment("Automatically stops TAS playback/recording when the level ends.");
    m_StopOnFinish->SetDefaultBoolean(true);

    // --- Hotkeys ---
    m_StopKey = config->GetProperty("Hotkeys", "StopKey");
    m_StopKey->SetComment("Key for stopping TAS playback or recording");
    m_StopKey->SetDefaultKey(CKKEY_F3);

    // --- Recording ---
    m_RecordingMaxFrames = config->GetProperty("Recording", "MaxFrames");
    m_RecordingMaxFrames->SetComment("Maximum frames to record (prevents memory issues)");
    m_RecordingMaxFrames->SetDefaultInteger(1000000);

    // --- Startup Script ---
    m_StartupScriptEnabled = config->GetProperty("Startup", "Enabled");
    m_StartupScriptEnabled->SetComment("Run a TAS project when the game starts");
    m_StartupScriptEnabled->SetDefaultBoolean(true);

    m_StartupScriptProject = config->GetProperty("Startup", "Project");
    m_StartupScriptProject->SetComment("Project to run at startup: a record or level script is armed and plays in its level; "
                                       "a global script runs when its trigger is startup. None picks a global script "
                                       "with trigger = startup");
    m_StartupScriptProject->SetDefaultString("");

    m_StartupLevel = config->GetProperty("Startup", "Level");
    m_StartupLevel->SetComment("Level to enter after the startup project starts; None waits for you "
                               "(level scripts default to their manifest level)");
    m_StartupLevel->SetDefaultString("");
    std::vector<std::string> levels{""};
    for (int level = LevelNames::kFirstLevel; level <= LevelNames::kLastLevel; ++level) {
        levels.push_back(LevelNames::ToKey(level));
    }
    SetChoiceEditor(m_StartupLevel, levels);

    m_AutoLoadStartupScript = config->GetProperty("Startup", "AutoLoad");
    m_AutoLoadStartupScript->SetComment("Start the startup project automatically when the game launches");
    m_AutoLoadStartupScript->SetDefaultBoolean(true);

    // --- OSD ---
    m_ShowOSD = config->GetProperty("OSD", "ShowOSD");
    m_ShowOSD->SetComment("Controls the visibility of the in-game On-Screen Display.");
    m_ShowOSD->SetDefaultBoolean(true);

    m_ShowOSDStatus = config->GetProperty("OSD", "ShowStatusPanel");
    m_ShowOSDStatus->SetComment("Show the status panel (TAS mode, playback frame, frame time, SR time, level)");
    m_ShowOSDStatus->SetDefaultBoolean(true);

    m_ShowOSDBall = config->GetProperty("OSD", "ShowBallPanel");
    m_ShowOSDBall->SetComment("Show the ball panel (position, velocity, per-frame velocity change)");
    m_ShowOSDBall->SetDefaultBoolean(true);

    m_ShowOSDInput = config->GetProperty("OSD", "ShowInputPanel");
    m_ShowOSDInput->SetComment("Show the input timeline (keys of the last and, during record playback, next frames)");
    m_ShowOSDInput->SetDefaultBoolean(true);

    m_ShowOSDValidation = config->GetProperty("OSD", "ShowValidationPanel");
    m_ShowOSDValidation->SetComment("Show the live comparison against the reference run while a playback validates");
    m_ShowOSDValidation->SetDefaultBoolean(true);

    m_ShowOSDWatches = config->GetProperty("OSD", "ShowWatchPanel");
    m_ShowOSDWatches->SetComment("Show the values scripts set with tas.hud.set");
    m_ShowOSDWatches->SetDefaultBoolean(true);

    m_OSDPositionX = config->GetProperty("OSD", "PositionX");
    m_OSDPositionX->SetComment("OSD horizontal position (0.0 = left, 1.0 = right)");
    m_OSDPositionX->SetDefaultFloat(0.02f);

    m_OSDPositionY = config->GetProperty("OSD", "PositionY");
    m_OSDPositionY->SetComment("OSD vertical position (0.0 = top, 1.0 = bottom)");
    m_OSDPositionY->SetDefaultFloat(0.02f);

    m_OSDOpacity = config->GetProperty("OSD", "Opacity");
    m_OSDOpacity->SetComment("OSD transparency (0.0 = transparent, 1.0 = opaque)");
    m_OSDOpacity->SetDefaultFloat(0.9f);

    m_OSDScale = config->GetProperty("OSD", "Scale");
    m_OSDScale->SetComment("OSD scale factor (1.0 = normal size)");
    m_OSDScale->SetDefaultFloat(1.0f);
}

void ConfigService::OnPropertyChanged(const char *category, const char *key, IProperty *prop) {
    if (!m_EventBus) return;

    if (prop == m_Enabled || prop == m_Validation || prop == m_AutoRestart || prop == m_PreserveLevelHistory || prop == m_SkipRenderUntilFrame ||
        prop == m_StopOnFinish || prop == m_StopKey || prop == m_RecordingMaxFrames ||
        prop == m_StartupScriptEnabled || prop == m_StartupScriptProject || prop == m_StartupLevel ||
        prop == m_AutoLoadStartupScript || prop == m_ShowOSD ||
        prop == m_ShowOSDStatus || prop == m_ShowOSDBall ||
        prop == m_ShowOSDInput || prop == m_ShowOSDValidation ||
        prop == m_ShowOSDWatches || prop == m_OSDPositionX || prop == m_OSDPositionY ||
        prop == m_OSDOpacity || prop == m_OSDScale) {
        m_EventBus->Publish(ConfigChangedEvent{category, key});
    }
}

StartupConfig ConfigService::GetStartupConfig() const {
    StartupConfig cfg;
    if (m_StartupScriptEnabled) cfg.enabled = m_StartupScriptEnabled->GetBoolean();
    if (m_StartupScriptProject) cfg.project = m_StartupScriptProject->GetString();
    if (m_StartupLevel) cfg.level = m_StartupLevel->GetString();
    if (m_AutoLoadStartupScript) cfg.autoLoad = m_AutoLoadStartupScript->GetBoolean();
    return cfg;
}

void ConfigService::SetStartupProjectChoices(const std::vector<std::string> &projectNames) {
    std::vector<std::string> choices{""};
    for (const std::string &name : projectNames) {
        if (!name.empty() && std::find(choices.begin(), choices.end(), name) == choices.end()) {
            choices.push_back(name);
        }
    }
    SetChoiceEditor(m_StartupScriptProject, choices);
}

OSDConfig ConfigService::GetOSDConfig() const {
    OSDConfig cfg;
    if (m_ShowOSDStatus)     cfg.showStatus     = m_ShowOSDStatus->GetBoolean();
    if (m_ShowOSDBall)       cfg.showBall       = m_ShowOSDBall->GetBoolean();
    if (m_ShowOSDInput)      cfg.showInput      = m_ShowOSDInput->GetBoolean();
    if (m_ShowOSDValidation) cfg.showValidation = m_ShowOSDValidation->GetBoolean();
    if (m_ShowOSDWatches)    cfg.showWatches    = m_ShowOSDWatches->GetBoolean();
    if (m_OSDPositionX)      cfg.positionX      = m_OSDPositionX->GetFloat();
    if (m_OSDPositionY)      cfg.positionY      = m_OSDPositionY->GetFloat();
    if (m_OSDOpacity)        cfg.opacity        = m_OSDOpacity->GetFloat();
    if (m_OSDScale)          cfg.scale          = m_OSDScale->GetFloat();
    return cfg;
}
