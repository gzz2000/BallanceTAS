#include "TASMenu.h"

#include "Logger.h"
#include "GameInterface.h"
#include "Recorder.h"
#include "TASEngine.h"
#include "ProjectManager.h"
#include "ScriptGenerator.h"
#include "ServiceContainer.h"
#include "TASProject.h"
#include "UIManager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

constexpr float kMenuTextX = 0.300f;
constexpr float kMenuButtonX = 0.4031f;
constexpr float kMenuWidthFraction = 0.40f;

static float ViewportWidth() {
    return ImGui::GetMainViewport()->Size.x;
}

static float MenuWidth(float fraction = 0.4f) {
    return ViewportWidth() * fraction;
}

static void TextBlock(float x, float y, const char *text, float width = 0.4f, float scale = 1.0f) {
    Bui::At(x, y, [&]() {
        Bui::WrappedText(text, MenuWidth(width), ImGui::GetCursorPosX(), scale);
    });
}

static void TextBlock(float x, float y, const std::string &text, float width = 0.4f, float scale = 1.0f) {
    TextBlock(x, y, text.c_str(), width, scale);
}

static ImVec4 TextColorForTone(TASMenuTone tone) {
    switch (tone) {
    case TASMenuTone::Good:
        return ImVec4(0.55f, 1.0f, 0.55f, 1.0f);
    case TASMenuTone::Warning:
        return ImVec4(1.0f, 0.72f, 0.28f, 1.0f);
    case TASMenuTone::Error:
        return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
    case TASMenuTone::Active:
        return ImVec4(0.3f, 0.85f, 1.0f, 1.0f);
    case TASMenuTone::Muted:
        return ImVec4(0.72f, 0.72f, 0.72f, 1.0f);
    case TASMenuTone::Normal:
    default:
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

static void ToneText(float x, float y, const std::string &text, TASMenuTone tone, float width = 0.4f, float scale = 1.0f) {
    ImGui::PushStyleColor(ImGuiCol_Text, TextColorForTone(tone));
    TextBlock(x, y, text, width, scale);
    ImGui::PopStyleColor();
}

template <typename Draw>
static void MenuItemAt(float x, float y, Draw &&draw) {
    Bui::At(x, y, std::forward<Draw>(draw));
}

static void DrawStateLine(float y, const TASMenuStatePresentation &state) {
    if (state.label == "Idle" && state.failureReason.empty()) {
        return;
    }

    std::string detail;
    if (state.label == "Pending Playback") {
        detail = state.detail.empty() ? "Waiting for playback" : state.detail;
    } else if (state.label == "Running Playback") {
        detail = state.detail.empty() ? "Playback running" : "Running: " + state.detail;
    } else if (state.label == "Pending Recording") {
        detail = state.detail.empty() ? "Waiting to record" : state.detail;
    } else if (state.label == "Pending Translation") {
        detail = state.detail.empty() ? "Preparing translation" : state.detail;
    } else {
        detail = state.detail.empty() ? state.label : state.label + ": " + state.detail;
    }

    ToneText(kMenuTextX, y, TruncateMenuLabel(detail, 36), state.tone, kMenuWidthFraction, 0.68f);
    if (!state.failureReason.empty() && state.failureReason != state.detail) {
        ToneText(kMenuTextX, y + 0.028f, "Error: " + state.failureReason, TASMenuTone::Error, kMenuWidthFraction, 0.70f);
    }
}

// TASMenuPage Implementation
TASMenuPage::TASMenuPage(TASMenu *menu, std::string name)
    : m_Menu(menu), m_Title(std::move(name)) {}

Bui::PageAction TASMenuPage::OnFrame() {
    m_Menu->m_FrameAction = Bui::PageAction::None();
    m_Menu->m_DrawingPage = true;
    try {
        OnPostBegin();
        if (m_Menu->m_FrameAction.IsNone()) OnDraw();
        if (m_Menu->m_FrameAction.IsNone() && Bui::NavBack())
            m_Menu->QueueAction(Bui::PageAction::Back());
    } catch (...) {
        m_Menu->m_DrawingPage = false;
        throw;
    }
    m_Menu->m_DrawingPage = false;
    return std::move(m_Menu->m_FrameAction);
}

// TASMenu Implementation
TASMenu::TASMenu(TASEngine *engine) : m_Engine(engine),
    m_Routes([this]() { OnOpen(); }, [this]() { OnClose(); }) {
    if (!m_Engine) {
        throw std::runtime_error("TASMenu requires valid TASEngine instances");
    }
}

TASMenu::~TASMenu() {
    Shutdown();
}

void TASMenu::Init() {
    if (m_Initialized) return;
    try {
        if (!m_Routes.CreatePage<TASListPage>("TAS Projects", this) ||
            !m_Routes.CreatePage<TASDetailsPage>("TAS Details", this) ||
            !m_Routes.CreatePage<TASRecordingPage>("Record New TAS", this))
            throw std::runtime_error("Could not register TAS menu routes");

        RefreshProjects();
        m_Initialized = true;
    } catch (const std::exception &e) {
        Log::Error("Failed to initialize TAS Menu: %s", e.what());
        throw;
    }
}

void TASMenu::Shutdown() {
    if (m_ShuttingDown) return;
    m_ShuttingDown = true;
    try {
        Close();
    } catch (const std::exception &e) {
        Log::Error("Exception during TAS Menu shutdown: %s", e.what());
    }
    m_Initialized = false;
}

bool TASMenu::IsOpen() const {
    return !m_PendingRoute.empty() || m_Routes.IsOpen();
}

bool TASMenu::Open(const std::string &route) {
    if (!m_Initialized || m_ShuttingDown || !m_Routes.HasPage(route)) return false;
    if (IsOpen()) return true;
    Bui::BlockKeyboardInput();
    m_InputBlocked = true;
    m_PendingRoute = route;
    m_OpenRequestFrame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1;
    return true;
}

bool TASMenu::Close() {
    if (m_DrawingPage) {
        QueueAction(Bui::PageAction::Close());
        return true;
    }
    if (!m_PendingRoute.empty()) {
        m_PendingRoute.clear();
        m_OpenRequestFrame = -1;
        OnClose();
        return true;
    }
    return m_Routes.Close();
}

bool TASMenu::Render() {
    if (!m_PendingRoute.empty() && (!ImGui::GetCurrentContext() ||
        ImGui::GetFrameCount() != m_OpenRequestFrame)) {
        std::string route = std::move(m_PendingRoute);
        m_PendingRoute.clear();
        m_OpenRequestFrame = -1;
        if (!m_Routes.Open(route)) { OnClose(); return false; }
    }
    return m_Routes.Render();
}

void TASMenu::QueueAction(Bui::PageAction action) {
    if (m_FrameAction.IsNone()) m_FrameAction = std::move(action);
}

void TASMenu::OpenPage(const std::string &route) {
    const bool root = route == "TAS Projects";
    if (m_DrawingPage) {
        QueueAction(root ? Bui::PageAction::Open(route) : Bui::PageAction::Push(route));
    } else if (root) {
        m_Routes.Open(route);
    } else {
        m_Routes.Push(route);
    }
}

void TASMenu::OpenPrevPage() {
    if (m_DrawingPage) QueueAction(Bui::PageAction::Back());
    else m_Routes.Back();
}

void TASMenu::SetLastActionError(std::string error) {
    m_LastActionError = std::move(error);
}

void TASMenu::ClearLastActionError() {
    m_LastActionError.clear();
}

TASMenuRuntimeSnapshot TASMenu::BuildRuntimeSnapshot() const {
    TASMenuRuntimeSnapshot snapshot;
    snapshot.pendingPlayback = m_Engine->IsPendingPlay();
    snapshot.runningPlayback = m_Engine->IsPlaying();
    snapshot.pendingRecording = m_Engine->IsPendingRecord();
    snapshot.recording = m_Engine->IsRecording();
    snapshot.pendingTranslation = m_Engine->IsPendingTranslate();
    snapshot.translating = m_Engine->IsTranslating();
    snapshot.currentTick = m_Engine->GetCurrentTick();
    snapshot.lastError = m_LastActionError;
    snapshot.lastInfo = m_Engine->GetLastTranslationResultMessage();
    snapshot.playbackCompleted = !m_Engine->GetLastCompletedPlaybackProjectName().empty();

    if (auto *project = GetCurrentProject()) {
        snapshot.activeProjectName = project->GetName();
        snapshot.activeProjectKey = project->GetPath();
        snapshot.activeProjectIsRecord = project->IsRecordProject();
        snapshot.activeTargetLevel = project->GetTargetLevel();
        if (snapshot.pendingPlayback) {
            if (!project->GetTargetLevel().empty()) {
                snapshot.activityDetail = "Waiting for " + FormatLevelLabel(project->GetTargetLevel());
            }
        } else if (snapshot.runningPlayback) {
            if (project->IsRecordProject()) {
                snapshot.frameCount = project->GetRecordFrameCount();
            }
        } else if (snapshot.pendingTranslation || snapshot.translating) {
            snapshot.activityDetail = project->IsRecordProject()
                ? "Translating to script: " + project->GetName()
                : "Translating to record: " + project->GetName();
        }
    } else if (snapshot.playbackCompleted) {
        snapshot.activeProjectName = m_Engine->GetLastCompletedPlaybackProjectName();
    }

    if (snapshot.pendingRecording) {
        if (auto *recorder = m_Engine->GetServiceProvider().Resolve<Recorder>()) {
            snapshot.activeTargetLevel = recorder->GetGenerationOptions().targetLevel;
        }
        if (snapshot.activityDetail.empty()) {
            snapshot.activityDetail = snapshot.activeTargetLevel.empty()
                ? "Waiting for target level"
                : "Waiting for " + FormatLevelLabel(snapshot.activeTargetLevel);
        }
    }
    if (snapshot.recording) {
        snapshot.frameCount = m_Engine->GetRecordingFrameCount();
    }

    return snapshot;
}

TASMenuStatePresentation TASMenu::GetStatePresentation() const {
    return BuildMenuStatePresentation(BuildRuntimeSnapshot());
}

void TASMenu::AnnouncePendingTAS() const {
    auto *game = m_Engine->GetGameInterface();
    if (!game) {
        return;
    }

    auto *uiManager = game->GetUIManager();
    const std::string stopKey = uiManager ? uiManager->GetStopHotkeyName() : "";
    const std::string message = BuildPendingMessage(BuildRuntimeSnapshot(), stopKey);
    if (!message.empty()) {
        game->PrintMessage(message.c_str());
    }
}

void TASMenu::OnOpen() {
    RefreshProjects();
}

void TASMenu::OnClose() {
    if (!m_InputBlocked) return;
    m_InputBlocked = false;
    if (m_ShuttingDown || m_Engine->GetGameInterface()->IsIngame())
        Bui::UnblockKeyboardAfterRelease();
    else
        Bui::TransitionToScriptAndUnblock("Menu_Start");
}

void TASMenu::RefreshProjects() {
    m_Engine->GetServiceProvider().Resolve<ProjectManager>()->RefreshProjects();
    ClearLastActionError();
}

TASProject *TASMenu::GetCurrentProject() const {
    return m_Engine->GetServiceProvider().Resolve<ProjectManager>()->GetCurrentProject();
}

void TASMenu::SetCurrentProject(TASProject *project) {
    m_Engine->GetServiceProvider().Resolve<ProjectManager>()->SetCurrentProject(project);
}

void TASMenu::PlayProject(TASProject *project) {
    if (!project || !project->IsValid()) {
        Log::Error("Cannot play invalid project.");
        SetLastActionError(project ? project->GetValidationMessage() : "No project selected");
        return;
    }

    // Stop any current TAS activity
    if (IsTASActive()) {
        StopTAS();
    }

    Log::Info("Playing TAS: %s", project->GetName().c_str());

    // Set the current project and start replay via TASEngine
    SetCurrentProject(project);

    if (m_Engine->StartReplay()) {
        ClearLastActionError();
        AnnouncePendingTAS();
        Close(); // Close menu so user can load a level
    } else {
        Log::Error("Failed to start replay from menu.");
        SetLastActionError("Failed to start playback");
        // Reset project selection on failure
        SetCurrentProject(nullptr);
    }
}

void TASMenu::StopTAS(bool clearProject) {
    bool wasTranslating = m_Engine->IsTranslating() || m_Engine->IsPendingTranslate();
    bool wasPlaying = m_Engine->IsPlaying() || m_Engine->IsPendingPlay();
    bool wasRecording = m_Engine->IsRecording() || m_Engine->IsPendingRecord();

    if (wasTranslating) {
        m_Engine->StopTranslation(clearProject);
        Log::Info("Translation stopped from menu.");
    } else if (wasPlaying) {
        m_Engine->StopReplay(clearProject);
        Log::Info("Replay stopped from menu.");
    } else if (wasRecording) {
        m_Engine->StopRecording();
        Log::Info("Recording stopped from menu.");
    }

    if (clearProject) {
        // Clear project selection after stopping
        if (wasPlaying || wasTranslating) {
            SetCurrentProject(nullptr);
        }

        if (wasRecording || wasTranslating) {
            // Refresh projects (new one might have been generated)
            RefreshProjects();
        }
    }

    OpenPage("TAS Projects");
}

void TASMenu::StartRecording() {
    // Stop any current TAS activity
    if (IsTASActive()) {
        StopTAS();
    }

    // Clear any selected project since we're starting fresh
    SetCurrentProject(nullptr);

    if (m_Engine->StartRecording()) {
        Log::Info("Recording setup from menu.");
        ClearLastActionError();
        AnnouncePendingTAS();
        Close(); // Close menu so user can load a level
    } else {
        Log::Error("Failed to setup recording from menu.");
        SetLastActionError("Failed to setup recording");
    }
}

void TASMenu::StopRecording() {
    if (m_Engine->IsRecording() || m_Engine->IsPendingRecord()) {
        m_Engine->StopRecording();
        Log::Info("Recording stopped from menu.");

        // Refresh projects as a new one might have been generated
        RefreshProjects();
        OpenPage("TAS Projects");
    }
}

void TASMenu::TranslateProject(TASProject *project) {
    if (!project || !project->IsValid()) {
        Log::Error("Cannot translate: invalid project.");
        SetLastActionError(project ? project->GetValidationMessage() : "No project selected");
        return;
    }

    if (project->IsRecordProject() && !project->CanBeTranslated()) {
        Log::Error("Cannot translate record: %s",
                                     project->GetTranslationCompatibilityMessage().c_str());
        SetLastActionError(project->GetTranslationCompatibilityMessage());
        return;
    }
    if (project->IsScriptProject() && !project->CanTranslateToRecord()) {
        Log::Error("Cannot translate script to record: %s",
                   project->GetScriptToRecordCompatibilityMessage().c_str());
        SetLastActionError(project->GetScriptToRecordCompatibilityMessage());
        return;
    }

    // Stop any current TAS activity
    if (IsTASActive()) {
        StopTAS();
    }

    Log::Info("Translating %s: %s (%.1f Hz)",
              project->IsRecordProject() ? "record to script" : "script to record",
              project->GetName().c_str(),
              project->GetUpdateRate());

    // Set the current project and start translation via TASEngine
    SetCurrentProject(project);

    if (m_Engine->StartTranslation()) {
        ClearLastActionError();
        Close(); // Close menu so user can load a level
    } else {
        Log::Error("Failed to start translation from menu.");
        SetLastActionError("Failed to start translation");
        // Reset project selection on failure
        SetCurrentProject(nullptr);
    }
}

void TASMenu::StopTranslation() {
    if (m_Engine->IsTranslating() || m_Engine->IsPendingTranslate()) {
        m_Engine->StopTranslation();
        Log::Info("Translation stopped from menu.");

        // Refresh projects as a new script might have been generated
        RefreshProjects();
        OpenPage("TAS Projects");
    }
}

bool TASMenu::IsTASActive() const {
    return m_Engine->IsPlaying() || m_Engine->IsPendingPlay() ||
        m_Engine->IsRecording() || m_Engine->IsPendingRecord() ||
        m_Engine->IsTranslating() || m_Engine->IsPendingTranslate();
}

// TASListPage Implementation
TASListPage::TASListPage(TASMenu *menu) : TASMenuPage(menu, "TAS Projects") {}

TASListPage::~TASListPage() = default;

void TASListPage::OnPostBegin() {
    if (!m_Menu || !m_Menu->GetEngine())
        return;

    Bui::Title(m_Title.c_str(), 0.095f, 1.25f);

    auto *projectManager = m_Menu->GetEngine()->GetServiceProvider().Resolve<ProjectManager>();
    const auto &projects = projectManager->GetProjects();

    m_Count = static_cast<int>(projects.size());
    m_Pagination.Update(m_Count, MAX_ENTRIES_PER_PAGE);
}

void TASListPage::OnDraw() {
    const auto state = m_Menu ? m_Menu->GetStatePresentation() : TASMenuStatePresentation{};

    // Draw main action buttons
    DrawMainButtons();
    DrawStateLine(0.255f, state);

    if (m_Count == 0) {
        ToneText(kMenuTextX, 0.36f, "No TAS projects found", TASMenuTone::Warning);
        if (m_Menu && m_Menu->GetEngine()) {
            TextBlock(kMenuTextX, 0.41f, "TAS path: " + m_Menu->GetEngine()->GetPath(), kMenuWidthFraction, 0.72f);
        }
        MenuItemAt(kMenuButtonX, 0.52f, [&]() {
            if (Bui::LevelButton("Refresh")) {
                m_Menu->RefreshProjects();
            }
        });
        return;
    }

    bool entryToggle = true;
    const int baseIndex = m_Pagination.GetFirstItem();

    Bui::Entries([
        &entryToggle,
        this,
        baseIndex,
        &state
    ](int slotIndex) {
        const int projectIndex = baseIndex + slotIndex;
        if (projectIndex >= m_Count) {
            return false;
        }

        return OnDrawEntry(static_cast<size_t>(projectIndex), slotIndex, state, &entryToggle);
    }, kMenuButtonX, 0.305f, 0.058f, MAX_ENTRIES_PER_PAGE);

    DrawPageNavigation();

    // Show TAS status if active
    if (m_Menu && (m_Menu->IsTASActive() || !state.failureReason.empty())) {
        DrawTASStatus(state);
    }
}

void TASListPage::DrawTASStatus(const TASMenuStatePresentation &state) {
    if (!m_Menu || !m_Menu->GetEngine()) return;

    if (!state.failureReason.empty() && !state.stopAction.enabled) {
        MenuItemAt(kMenuButtonX, 0.780f, [&]() {
            if (Bui::LevelButton(state.refreshAction.label.c_str())) {
                m_Menu->RefreshProjects();
            }
        });
        return;
    }

    if (!state.stopAction.enabled) {
        return;
    }

    MenuItemAt(kMenuButtonX, 0.780f, [&]() {
        if (Bui::LevelButton(state.stopAction.label.c_str())) {
            if (state.label == "Translating" || state.label == "Pending Translation") {
                m_Menu->StopTranslation();
            } else if (state.label == "Recording" || state.label == "Pending Recording") {
                m_Menu->StopRecording();
            } else {
                m_Menu->StopTAS();
            }
        }
    });
}

void TASListPage::DrawMainButtons() {
    if (!m_Menu || !m_Menu->GetEngine()) return;

    auto *engine = m_Menu->GetEngine();
    bool canRecord = engine && !engine->IsPlaying() && !engine->IsRecording() &&
        !engine->IsPendingPlay() && !engine->IsPendingRecord() &&
        !engine->IsTranslating() && !engine->IsPendingTranslate();

    MenuItemAt(kMenuButtonX, 0.205f, [&]() {
        if (!canRecord) {
            ImGui::BeginDisabled();
        }

        if (Bui::LevelButton("Record TAS")) {
            m_Menu->OpenPage("Record New TAS");
        }

        if (!canRecord) {
            ImGui::EndDisabled();
        }
    });
}

void TASListPage::DrawPageNavigation() {
    if (m_Pagination.GetPageCount() <= 1) {
        return;
    }

    if (m_Pagination.CanPrevious() && Bui::NavLeft(0.360f, 0.430f)) {
        m_Pagination.Previous();
    }

    char pageText[32] = {};
    std::snprintf(pageText, sizeof(pageText), "%d/%d", m_Pagination.GetPage() + 1, m_Pagination.GetPageCount());
    TextBlock(0.486f, 0.708f, pageText, 0.04f, 0.70f);

    if (m_Pagination.CanNext() && Bui::NavRight(0.624f, 0.430f)) {
        m_Pagination.Next();
    }
}

bool TASListPage::OnDrawEntry(size_t index, int slotIndex, const TASMenuStatePresentation &state, bool *v) {
    if (!m_Menu) return false;

    auto *engine = m_Menu->GetEngine();
    auto *projectManager = engine ? engine->GetServiceProvider().Resolve<ProjectManager>() : nullptr;
    if (!engine || !projectManager)
        return false;
    const auto &projects = projectManager->GetProjects();

    if (index >= projects.size())
        return false;

    auto &project = projects[index];
    const auto presentation = BuildProjectPresentation(*project, state);

    // Visual indicator for project kind and state
    if (!project->IsValid()) {
        ImGui::PushStyleColor(ImGuiCol_Text, TextColorForTone(TASMenuTone::Error));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.2f, 0.2f, 0.6f));
    } else if (presentation.rowStatusLabel == "RUN" || presentation.rowStatusLabel == "WAIT") {
        ImGui::PushStyleColor(ImGuiCol_Text, TextColorForTone(presentation.rowTone));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.45f, 0.55f, 0.7f));
    } else if (project->IsRecordProject()) {
        // Record projects - purple/magenta
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.3f, 0.6f, 0.6f));
    } else if (project->IsZipProject()) {
        // Zip script projects - blue
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 0.6f));
    } else {
        // Regular script projects - default colors
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 0.6f));
    }

    ImGui::PushID(static_cast<int>(index));
    const std::string buttonLabel = presentation.displayName + "##tas_project_" + std::to_string(index);
    bool clicked = Bui::LevelButton(buttonLabel.c_str(), v);
    ImGui::PopID();

    ImGui::PopStyleColor(2);

    const float statusY = 0.312f + static_cast<float>(slotIndex) * 0.058f;
    ToneText(0.585f, statusY, presentation.rowBadgeLabel, presentation.rowTone, 0.035f, 0.62f);

    if (clicked) {
        m_Menu->SetCurrentProject(project.get());
        m_Menu->OpenPage("TAS Details");
    }

    return true;
}

// TASDetailsPage Implementation
TASDetailsPage::TASDetailsPage(TASMenu *menu) : TASMenuPage(menu, "TAS Details") {}

TASDetailsPage::~TASDetailsPage() = default;

void TASDetailsPage::OnPostBegin() {
    if (!m_Menu)
        return;

    auto *project = m_Menu->GetCurrentProject();
    if (!project) {
        m_Menu->SetLastActionError("Current project is no longer available");
        m_Menu->OpenPage("TAS Projects");
        return;
    }

    Bui::Title(TruncateMenuLabel(project->GetName(), 28).c_str(), 0.095f, 1.20f);
}

void TASDetailsPage::OnDraw() {
    if (!m_Menu) return;

    auto *project = m_Menu->GetCurrentProject();
    if (!project)
        return;

    DrawProjectInfo();
    DrawActionButtons();
}

void TASDetailsPage::DrawProjectInfo() {
    if (!m_Menu) return;

    auto *project = m_Menu->GetCurrentProject();
    if (!project) return;

    const auto state = m_Menu->GetStatePresentation();
    const auto presentation = BuildProjectPresentation(*project, state);
    float y = 0.165f;

    auto line = [&](const std::string &text, TASMenuTone tone = TASMenuTone::Normal, float scale = 0.88f) {
        ToneText(kMenuTextX, y, text, tone, kMenuWidthFraction, scale);
        y += 0.031f;
    };

    if (presentation.displayName != presentation.fullName) {
        line("Name: " + presentation.fullName, TASMenuTone::Normal, 0.78f);
    }
    line("Type: " + presentation.typeLabel);
    line("Scope: " + presentation.scopeLabel, project->IsGlobalProject() ? TASMenuTone::Good : TASMenuTone::Normal);
    line("Trigger: " + presentation.triggerLabel);
    line("Level: " + presentation.levelLabel);
    if (project->IsScriptProject()) {
        line("Entry: " + presentation.entryLabel);
    }
    line("Rate: " + presentation.updateRateLabel);

    if (project->IsRecordProject() || project->IsScriptProject()) {
        line("Frames: " + presentation.recordInfoLabel,
             project->GetRecordFrameCount() == 0 ? TASMenuTone::Error : TASMenuTone::Normal);
        line("Timing: " + presentation.translationLabel,
             project->CanBeTranslated() ? TASMenuTone::Good : TASMenuTone::Warning);
    }

    line("Status: " + presentation.validityLabel,
         project->IsValid() ? TASMenuTone::Good : TASMenuTone::Error,
         0.82f);

    if (!state.failureReason.empty()) {
        line("Last error: " + state.failureReason, TASMenuTone::Error, 0.78f);
    }
}

void TASDetailsPage::DrawActionButtons() {
    if (!m_Menu || !m_Menu->GetEngine()) return;

    auto *project = m_Menu->GetCurrentProject();
    const auto state = m_Menu->GetStatePresentation();
    const auto presentation = BuildProjectPresentation(*project, state);

    if (state.stopAction.enabled) {
        DrawStateLine(0.575f, state);
        MenuItemAt(kMenuButtonX, 0.655f, [&]() {
            if (Bui::LevelButton(state.stopAction.label.c_str())) {
                if (state.label == "Translating" || state.label == "Pending Translation") {
                    m_Menu->StopTranslation();
                } else if (state.label == "Recording" || state.label == "Pending Recording") {
                    m_Menu->StopRecording();
                } else {
                    m_Menu->StopTAS();
                }
            }
        });
        return;
    }

    MenuItemAt(kMenuButtonX, 0.625f, [&]() {
        if (!presentation.playAction.enabled) {
            ImGui::BeginDisabled();
        }

        if (Bui::LevelButton(presentation.playAction.label.c_str())) {
            m_Menu->PlayProject(project);
        }

        if (!presentation.playAction.enabled) {
            ImGui::EndDisabled();
        }
    });

    if (project->IsRecordProject()) {
        MenuItemAt(kMenuButtonX, 0.705f, [&]() {
            if (!presentation.translateAction.enabled) {
                ImGui::BeginDisabled();
            }

            if (Bui::LevelButton(presentation.translateAction.label.c_str())) {
                m_Menu->TranslateProject(project);
            }

            if (!presentation.translateAction.enabled) {
                ImGui::EndDisabled();
            }
        });
    }

    float reasonY = 0.795f;
    if (!presentation.playAction.enabled) {
        ToneText(kMenuTextX, reasonY, "Cannot play: " + presentation.playAction.disabledReason,
                 TASMenuTone::Error, kMenuWidthFraction, 0.72f);
    } else if (!presentation.translateAction.enabled) {
        ToneText(kMenuTextX, reasonY, "Cannot translate: " + presentation.translateAction.disabledReason,
                 TASMenuTone::Warning, kMenuWidthFraction, 0.72f);
    } else {
        ToneText(kMenuTextX, reasonY, presentation.nextStepLabel, TASMenuTone::Good, kMenuWidthFraction, 0.78f);
    }
}

// ===================================================================
// TASRecordingPage Implementation
// ===================================================================

TASRecordingPage::TASRecordingPage(TASMenu *menu) : TASMenuPage(menu, "Record New TAS") {
    if (!m_Menu || !m_Menu->GetEngine()) return;

    // Initialize with current level if available
    auto *engine = m_Menu->GetEngine();
    if (engine && engine->GetGameInterface()) {
        std::string currentMap = engine->GetGameInterface()->GetMapName();

        // Try to match current map to level options
        for (int i = 0; i < LEVEL_COUNT - 1; ++i) {
            if (currentMap.find(LEVEL_OPTIONS[i]) != std::string::npos ||
                currentMap == LEVEL_OPTIONS[i]) {
                m_TargetLevelIndex = i;
                break;
            }
        }
    }
}

void TASRecordingPage::OnPostBegin() {
    Bui::Title("Record New TAS", 0.095f, 1.25f);
}

void TASRecordingPage::OnDraw() {
    if (!m_Menu) return;

    auto *engine = m_Menu->GetEngine();
    if (!engine) return;

    // Check if we can record
    bool canRecord = !engine->IsPlaying() && !engine->IsRecording() &&
        !engine->IsPendingPlay() && !engine->IsPendingRecord() &&
        !engine->IsTranslating() && !engine->IsPendingTranslate();

    if (!canRecord) {
        const auto state = m_Menu->GetStatePresentation();
        DrawStateLine(0.240f, state);

        if (engine->IsPlaying() || engine->IsPendingPlay() ||
            engine->IsTranslating() || engine->IsPendingTranslate()) {
            ToneText(kMenuTextX, 0.315f, "Cannot record: Stop current TAS first", TASMenuTone::Error);
        } else if (engine->IsRecording() || engine->IsPendingRecord()) {
            ToneText(kMenuTextX, 0.315f, "Recording in progress or pending", TASMenuTone::Warning);
            MenuItemAt(kMenuButtonX, 0.420f, [&]() {
                if (Bui::LevelButton("Stop Recording")) {
                    StopRecording();
                }
            });
        }
        return;
    }

    DrawRecordingControls();
    DrawGenerationOptions();
    DrawStartButton();
}

void TASRecordingPage::DrawRecordingControls() {
    if (!m_Menu) return;

    TextBlock(kMenuTextX, 0.165f, "Project Settings", kMenuWidthFraction, 0.90f);

    MenuItemAt(0.355f, 0.225f, [&]() {
        Bui::InputTextButton("Name", m_ProjectName, sizeof(m_ProjectName));
    });

    MenuItemAt(0.355f, 0.315f, [&]() {
        Bui::InputTextButton("Author", m_AuthorName, sizeof(m_AuthorName));
    });

    MenuItemAt(0.355f, 0.405f, [&]() {
        Bui::RadioButton("Level", &m_TargetLevelIndex, LEVEL_OPTIONS, LEVEL_COUNT);
    });

    MenuItemAt(0.355f, 0.495f, [&]() {
        Bui::InputFloatButton("Rate", &m_UpdateRate, 0.0f, 0.0f, "%.3f");
    });

    MenuItemAt(0.355f, 0.585f, [&]() {
        Bui::InputTextButton("Description", m_Description, sizeof(m_Description));
    });
}

void TASRecordingPage::DrawGenerationOptions() {
    if (!m_Menu) return;

    MenuItemAt(0.355f, 0.675f, [&]() {
        Bui::YesNoButton("Frame Comments", &m_AddFrameComments);
    });
}

void TASRecordingPage::DrawStartButton() {
    if (!m_Menu) return;

    const std::string output = std::string("Output: ") + m_ProjectName;
    ToneText(kMenuTextX, 0.745f, output, TASMenuTone::Muted, kMenuWidthFraction, 0.68f);

    MenuItemAt(kMenuButtonX, 0.785f, [&]() {
        if (Bui::LevelButton("Start Recording")) {
            StartRecording();
        }
    });
}

void TASRecordingPage::StartRecording() {
    if (!m_Menu) return;

    auto *engine = m_Menu->GetEngine();
    if (!engine) return;

    // Validate and clean project name
    std::string projectName = m_ProjectName;
    if (projectName.empty()) {
        projectName = "TAS_Untitled";
        std::snprintf(m_ProjectName, sizeof(m_ProjectName), "%s", projectName.c_str());
    }

    // Replace invalid characters
    std::replace_if(projectName.begin(), projectName.end(),
                    [](char c) {
                        return c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' ||
                            c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
                    },
                    '_');

    // Update the field with cleaned name
    std::snprintf(m_ProjectName, sizeof(m_ProjectName), "%s", projectName.c_str());

    // Get target level string
    std::string targetLevel = "Level_01";
    if (m_TargetLevelIndex >= 0 && m_TargetLevelIndex < LEVEL_COUNT) {
        targetLevel = LEVEL_OPTIONS[m_TargetLevelIndex];
    }

    // Configure recorder with settings from UI
    if (auto *recorder = engine->GetServiceProvider().Resolve<Recorder>()) {
        // Create generation options from UI settings
        GenerationOptions options;
        options.projectName = projectName;
        options.authorName = m_AuthorName;
        options.targetLevel = targetLevel;
        options.description = m_Description;
        options.updateRate = m_UpdateRate;
        options.addFrameComments = m_AddFrameComments;

        // Set the generation options on the recorder
        recorder->SetGenerationOptions(options);
        recorder->SetUpdateRate(m_UpdateRate);
    }

    if (engine->StartRecording()) {
        Log::Info("Recording setup for project: %s", projectName.c_str());
        Log::Info("  Author: %s", m_AuthorName);
        Log::Info("  Target Level: %s", targetLevel.c_str());
        Log::Info("  Update Rate: %.3f Hz", m_UpdateRate);
        Log::Info("  Description: %s", m_Description);
        Log::Info("  Generation Options: frameComments=%s",
                                  m_AddFrameComments ? "true" : "false");
        m_Menu->AnnouncePendingTAS();
        m_Menu->Close();
    } else {
        Log::Error("Failed to setup recording.");
        m_Menu->SetLastActionError("Failed to setup recording");
    }
}

void TASRecordingPage::StopRecording() {
    if (!m_Menu) return;

    auto *engine = m_Menu->GetEngine();
    if (!engine || (!engine->IsRecording() && !engine->IsPendingRecord())) return;

    engine->StopRecording();
    Log::Info("Recording stopped from recording page.");
    m_Menu->OpenPrevPage();
}
