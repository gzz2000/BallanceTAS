#ifndef BML_TASMENU_H
#define BML_TASMENU_H

#include <string>
#include <memory>

#include <BML/Bui.h>

#include "TASMenuPresentation.h"

class TASEngine;
class TASProject;
class TASMenu;

class TASMenuPage : public Bui::Page {
public:
    TASMenuPage(TASMenu *menu, std::string name);
    ~TASMenuPage() override = default;

protected:
    Bui::PageAction OnFrame() final;
    virtual void OnPostBegin() {}
    virtual void OnDraw() = 0;
    TASMenu *m_Menu;
    std::string m_Title;
};

class TASMenu {
public:
    explicit TASMenu(TASEngine *engine);
    ~TASMenu();

    // TASMenu is not copyable or movable
    TASMenu(const TASMenu &) = delete;
    TASMenu &operator=(const TASMenu &) = delete;

    void Init();
    void Shutdown();

    bool Open(const std::string &route);
    bool Close();
    bool Render();
    void OpenPage(const std::string &route);
    void OpenPrevPage();

    // Project management
    void RefreshProjects();
    TASProject *GetCurrentProject() const;
    void SetCurrentProject(TASProject *project);

    // Actions
    void PlayProject(TASProject *project);
    void StopTAS(bool clearProject = true);
    void StartRecording();
    void StopRecording();
    void TranslateProject(TASProject *project);
    void StopTranslation();

    // State queries
    bool IsTASActive() const;
    bool IsOpen() const;
    TASMenuStatePresentation GetStatePresentation() const;
    /// Tells the player in-game what to do next once playback/recording is armed and waiting for a level.
    void AnnouncePendingTAS() const;
    const std::string &GetLastActionError() const { return m_LastActionError; }
    void SetLastActionError(std::string error);
    void ClearLastActionError();

    TASEngine *GetEngine() const { return m_Engine; }

private:
    friend class TASMenuPage;
    void OnOpen();
    void OnClose();
    void QueueAction(Bui::PageAction action);
    TASMenuRuntimeSnapshot BuildRuntimeSnapshot() const;

    TASEngine *m_Engine;
    std::string m_LastActionError;
    bool m_Initialized = false;
    bool m_ShuttingDown = false;
    bool m_InputBlocked = false;
    bool m_DrawingPage = false;
    int m_OpenRequestFrame = -1;
    std::string m_PendingRoute;
    Bui::PageAction m_FrameAction;
    // Routes own the pages, and must die before the state they reference.
    Bui::Menu m_Routes;
};

class TASListPage : public TASMenuPage {
public:
    explicit TASListPage(TASMenu *menu);
    ~TASListPage() override;

    void OnPostBegin() override;
    void OnDraw() override;

private:
    bool OnDrawEntry(size_t index, int slotIndex, const TASMenuStatePresentation &state, bool *v);
    void DrawMainButtons();
    void DrawPageNavigation();
    void DrawTASStatus(const TASMenuStatePresentation &state);

    int m_Count = 0;
    Bui::Pagination m_Pagination;
    static constexpr int MAX_ENTRIES_PER_PAGE = 7;
};

class TASDetailsPage : public TASMenuPage {
public:
    explicit TASDetailsPage(TASMenu *menu);
    ~TASDetailsPage() override;

    void OnPostBegin() override;
    void OnDraw() override;

private:
    void DrawProjectInfo();
    void DrawActionButtons();

    char m_TextBuf[1024] = {};
};

/**
 * @class TASRecordingPage
 * @brief UI page for starting and configuring recordings.
 */
class TASRecordingPage : public TASMenuPage {
public:
    explicit TASRecordingPage(TASMenu *menu);
    ~TASRecordingPage() override = default;

    void OnPostBegin() override;
    void OnDraw() override;

private:
    void DrawRecordingControls();
    void DrawGenerationOptions();
    void DrawStartButton();

    void StartRecording();
    void StopRecording();

    // Project configuration
    char m_ProjectName[256] = "TAS_Untitled";
    char m_AuthorName[128] = "Player";
    char m_Description[512] = "Recorded TAS run";
    int m_TargetLevelIndex = 0;
    float m_UpdateRate = 132.0f;

    // Generation options
    bool m_AddFrameComments = true;

    // Level selection constants
    static constexpr const char *LEVEL_OPTIONS[] = {
        "Level_01", "Level_02", "Level_03", "Level_04", "Level_05",
        "Level_06", "Level_07", "Level_08", "Level_09", "Level_10",
        "Level_11", "Level_12", "Level_13", "Custom"
    };
    static constexpr int LEVEL_COUNT = sizeof(LEVEL_OPTIONS) / sizeof(LEVEL_OPTIONS[0]);
};

#endif // BML_TASMENU_H
