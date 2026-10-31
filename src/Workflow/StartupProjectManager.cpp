#include "StartupProjectManager.h"

#include "GameInterface.h"
#include "physics_RT.h"
#include "LevelNames.h"
#include "Logger.h"
#include "TASEngine.h"
#include "TASProject.h"
#include "ProjectManager.h"
#include "ScriptContext.h"
#include "ScriptContextManager.h"
#include "ServiceContainer.h"

StartupProjectManager::StartupProjectManager(TASEngine *engine) : m_Engine(engine) {}

StartupProjectManager::~StartupProjectManager() {
    Shutdown();
}

bool StartupProjectManager::Initialize() {
    if (m_Initialized) {
        return true;
    }

    if (!m_Engine) {
        return false;
    }

    m_Initialized = true;
    return true;
}

void StartupProjectManager::Shutdown() {
    if (!m_Initialized) {
        return;
    }

    m_StartupProject.reset();
    m_StartupProjectName.clear();
    m_HasExecutedStartup = false;
    m_PendingStep = PendingStep::None;
    m_Initialized = false;
}

bool StartupProjectManager::SetStartupProject(const std::string &projectName) {
    if (!m_Initialized) {
        return false;
    }

    if (m_StartupProjectName != projectName) {
        m_StartupProjectName = projectName;
        m_HasExecutedStartup = false; // Reset execution flag when project changes
        return RefreshStartupProject();
    }

    return HasStartupProject();
}


bool StartupProjectManager::LoadAndExecuteStartupScript() {
    if (!m_Initialized || !m_StartupEnabled || !m_AutoLoadEnabled) {
        return false;
    }

    m_PendingStep = PendingStep::None;
    if (TASProject *replay = FindReplayProject()) {
        // Records and level scripts play through the engine, armed before the level loads.
        m_ReplayProjectName = replay->GetName();
        m_PendingStep = PendingStep::Arm;
        m_PendingFrames = 0;
        const auto projectLevel = LevelNames::Parse(replay->GetTargetLevel());
        ScheduleLevel(projectLevel ? *projectLevel : 0);
        return true;
    }

    const bool started = LoadStartupProject() && ExecuteStartupProjectIfAppropriate("startup");
    ScheduleLevel(0);
    return started;
}

TASProject *StartupProjectManager::FindReplayProject() const {
    if (m_StartupProjectName.empty()) {
        return nullptr;
    }
    auto *projectManager = m_Engine->GetServiceProvider().Resolve<ProjectManager>();
    if (!projectManager) {
        return nullptr;
    }
    TASProject *project = projectManager->FindProject(m_StartupProjectName);
    // Records are also accepted by file name ("SR_01.tas").
    const std::string extension = ".tas";
    const std::string &name = m_StartupProjectName;
    if (!project && name.size() > extension.size() &&
        name.compare(name.size() - extension.size(), extension.size(), extension) == 0) {
        project = projectManager->FindProject(name.substr(0, name.size() - extension.size()));
    }
    if (!project) {
        return nullptr;
    }
    const bool replayable = project->IsRecordProject() ||
                            (project->IsScriptProject() && !project->IsGlobalProject());
    return replayable && project->IsValid() ? project : nullptr;
}

// Startup.Level wins; otherwise the project's own level, if it has one.
void StartupProjectManager::ScheduleLevel(int projectLevel) {
    m_LevelToLoad = projectLevel;
    if (!m_StartupLevelName.empty()) {
        if (const auto level = LevelNames::Parse(m_StartupLevelName)) {
            m_LevelToLoad = *level;
        } else {
            Log::Warn("Startup.Level '%s' is not a level; expected Level_01 ... Level_13.",
                      m_StartupLevelName.c_str());
        }
    }
    if (m_LevelToLoad != 0 && m_PendingStep == PendingStep::None) {
        m_PendingStep = PendingStep::LoadLevel;
        m_PendingFrames = 0;
    }
}

void StartupProjectManager::Tick() {
    switch (m_PendingStep) {
    case PendingStep::Arm:
    case PendingStep::WaitArmed:
        TickArm();
        break;
    case PendingStep::LoadLevel:
        TickLoadLevel();
        break;
    case PendingStep::None:
        break;
    }
}

void StartupProjectManager::TickArm() {
    if (m_PendingStep == PendingStep::Arm) {
        auto *projectManager = m_Engine->GetServiceProvider().Resolve<ProjectManager>();
        TASProject *project = projectManager ? projectManager->FindProject(m_ReplayProjectName) : nullptr;
        if (!project) {
            Log::Error("Startup project '%s' disappeared before it could be armed.", m_ReplayProjectName.c_str());
            m_PendingStep = PendingStep::None;
            return;
        }
        projectManager->SetCurrentProject(project);
        if (!m_Engine->StartReplay()) {
            Log::Error("Could not arm startup project '%s' for playback.", m_ReplayProjectName.c_str());
            m_PendingStep = PendingStep::None;
            return;
        }
        m_PendingStep = PendingStep::WaitArmed;
        m_PendingFrames = 0;
        return;
    }

    // Arming completes on the engine's own tick.
    if (m_Engine->IsPendingPlay() || m_Engine->IsPlaying()) {
        m_HasExecutedStartup = true;
        if (m_LevelToLoad != 0) {
            Log::Info("Startup project '%s' armed; entering %s.", m_ReplayProjectName.c_str(),
                      LevelNames::ToKey(m_LevelToLoad).c_str());
            m_PendingStep = PendingStep::LoadLevel;
            m_PendingFrames = 0;
        } else {
            Log::Info("Startup project '%s' armed; it plays when you start a level.", m_ReplayProjectName.c_str());
            m_PendingStep = PendingStep::None;
        }
    } else if (m_Engine->IsIdle() && ++m_PendingFrames > 10) {
        Log::Error("Startup project '%s' was not armed for playback.", m_ReplayProjectName.c_str());
        m_PendingStep = PendingStep::None;
    }
}

// The menu becomes usable a few frames after the game reaches it, so keep trying.
void StartupProjectManager::TickLoadLevel() {
    // This counter advances on simulation ticks (normally 264 Hz), not display
    // frames.  The old 20*60 limit expired after about 4.5 seconds, during the
    // unskippable startup sequence, before Menu_Start existed.
    constexpr int kMaxFrames = 60 * 264;
    GameInterface *game = m_Engine->GetGameInterface();
    if (!game || game->IsIngame()) {
        // No game, or the player got there first.
        m_PendingStep = PendingStep::None;
        return;
    }
    std::string reason;
    if (game->LoadLevel(m_LevelToLoad, reason)) {
        Log::Info("Startup: loading %s.", LevelNames::ToKey(m_LevelToLoad).c_str());
        m_PendingStep = PendingStep::None;
        return;
    }
    if (++m_PendingFrames >= kMaxFrames) {
        Log::Warn("Startup: could not enter %s: %s", LevelNames::ToKey(m_LevelToLoad).c_str(), reason.c_str());
        m_PendingStep = PendingStep::None;
    }
}

void StartupProjectManager::OnGameStart() {
    if (!m_Initialized || !m_StartupEnabled) {
        return;
    }

    // For startup-triggered projects
    if (m_AutoLoadEnabled && !m_HasExecutedStartup) {
        LoadAndExecuteStartupScript();
    }
}

void StartupProjectManager::OnEnterMainMenu() {
    if (!m_Initialized || !m_StartupEnabled) {
        return;
    }

    // Execute projects that should trigger on menu entry
    if (LoadProjectForContext("menu")) {
        ExecuteStartupProjectIfAppropriate("menu");
    }
}

void StartupProjectManager::OnEnterLevel(const std::string &levelName) {
    if (!m_Initialized || !m_StartupEnabled) {
        return;
    }

    // Execute projects that should trigger on level entry
    // This allows global projects to work in specific levels too
    if (LoadProjectForContext("level", levelName)) {
        ExecuteStartupProjectIfAppropriate("level", levelName);
    }
}

bool StartupProjectManager::RefreshStartupProject() {
    if (!m_Initialized) {
        m_StartupProject.reset();
        return false;
    }

    if (m_StartupProjectName.empty()) {
        m_StartupProject.reset();
        return false;
    }

    auto *projectManager = m_Engine->GetServiceProvider().Resolve<ProjectManager>();
    if (!projectManager) {
        m_StartupProject.reset();
        return false;
    }

    const auto &allProjects = projectManager->GetProjects();
    for (const auto &project : allProjects) {
        if (project && project->GetName() == m_StartupProjectName &&
            project->IsScriptProject() && project->IsGlobalProject() && project->IsValid()) {
            m_StartupProject = std::make_unique<TASProject>(*project);
            return true;
        }
    }

    m_StartupProject.reset();
    return false;
}

std::vector<std::string> StartupProjectManager::GetAvailableGlobalProjects() const {
    std::vector<std::string> globalProjects;

    if (!m_Initialized || !m_Engine) {
        return globalProjects;
    }

    auto *projectManager = m_Engine->GetServiceProvider().Resolve<ProjectManager>();
    if (!projectManager) {
        return globalProjects;
    }

    // Get all projects and filter for global ones
    const auto &allProjects = projectManager->GetProjects();
    for (const auto &project : allProjects) {
        if (project && project->IsGlobalProject() && project->IsValid()) {
            globalProjects.push_back(project->GetName());
        }
    }

    return globalProjects;
}

bool StartupProjectManager::LoadStartupProject() {
    return LoadProjectForContext("startup");
}

// A configured name that can never run would otherwise fail silently.
void StartupProjectManager::WarnUnusableStartupProject(const std::vector<std::unique_ptr<TASProject>> &projects) const {
    for (const auto &project : projects) {
        if (!project || project->GetName() != m_StartupProjectName) {
            continue;
        }
        if (!project->IsValid()) {
            Log::Warn("Startup project '%s' is not valid.", m_StartupProjectName.c_str());
        } else if (project->IsScriptProject() && project->IsGlobalProject()) {
            Log::Warn("Startup project '%s' is a global script with trigger = \"%s\"; only trigger = \"startup\" "
                      "runs at startup.", m_StartupProjectName.c_str(), project->GetExecutionTrigger().c_str());
        }
        return;
    }
    Log::Warn("Startup project '%s' was not found; use the project name without its file extension, "
              "or leave Startup.Project empty to pick a global project with trigger = \"startup\".",
              m_StartupProjectName.c_str());
}

bool StartupProjectManager::LoadProjectForContext(const std::string &context, const std::string &levelName) {
    if (!m_Initialized) {
        return false;
    }

    auto *projectManager = m_Engine->GetServiceProvider().Resolve<ProjectManager>();
    if (!projectManager) {
        return false;
    }

    const auto &allProjects = projectManager->GetProjects();
    if (m_StartupProjectName.empty() && context != "startup") {
        m_StartupProject.reset();
        return false;
    }

    TASProject *selected = SelectProjectForContext(allProjects, m_StartupProjectName, context, levelName);
    if (!selected) {
        if (context == "startup" && !m_StartupProjectName.empty()) {
            WarnUnusableStartupProject(allProjects);
        }
        m_StartupProject.reset();
        return false;
    }

    m_StartupProject = std::make_unique<TASProject>(*selected);
    return true;
}

bool StartupProjectManager::ExecuteStartupProjectIfAppropriate(const std::string &context, const std::string &levelName) {
    if (!m_Initialized || !m_StartupEnabled || !m_StartupProject) {
        return false;
    }

    if (!IsProjectEligibleForContext(*m_StartupProject, context, levelName)) {
        return false;
    }

    auto *contextManager = m_Engine->GetServiceProvider().Resolve<ScriptContextManager>();
    if (!contextManager) {
        Log::Error("ScriptContextManager not available for startup project execution.");
        return false;
    }

    try {
        // Get or create the global context
        auto globalContext = contextManager->GetOrCreateGlobalContext();
        if (!globalContext) {
            Log::Error("Failed to get global context for startup project.");
            return false;
        }

        // Load and execute the startup project in the global context
        bool success = globalContext->LoadAndExecute(m_StartupProject.get());
        if (success) {
            m_HasExecutedStartup = true;
            Log::Info("Successfully loaded startup project '%s' into global context.",
                      m_StartupProject->GetName().c_str());
            return true;
        } else {
            Log::Error("Failed to load startup project '%s' into global context.",
                       m_StartupProject->GetName().c_str());
        }
    } catch (const std::exception &e) {
        // Log error but don't crash
        Log::Error("Exception while executing startup project '%s': %s",
                   m_StartupProject->GetName().c_str(), e.what());
    }

    return false;
}
