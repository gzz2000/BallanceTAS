#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "PlaybackTypes.h"
#include "Result.h"

class EventBus;
class TASProject;

class RuntimeSession {
public:
    enum class State {
        Idle,
        PendingRecord,
        Recording,
        PendingScriptPlayback,
        PendingRecordPlayback,
        PlayingScript,
        PlayingRecord,
        PendingTranslation,
        Translating,
        Paused,
        ShuttingDown
    };

    enum class Event {
        StartRecording,
        StartScriptPlayback,
        StartRecordPlayback,
        StartTranslation,
        Stop,
        Pause,
        Resume,
        LevelLoadStart,
        LevelStart,
        LevelEnd,
        Shutdown,
        Error
    };

    struct RecordingOptions {
        bool validation = false;
    };

    struct PlaybackOptions {
        bool validation = false;
    };

    struct TranslationOptions {
    };

    struct StopOptions {
        bool clearProject = false;
    };

    struct SessionSnapshot {
        State state = State::Idle;
        State pausedFromState = State::Idle;
        PlaybackType playbackType = PlaybackType::None;
        bool isIdle = true;
        bool isRecording = false;
        bool isPlaying = false;
        bool isTranslating = false;
        bool isPaused = false;
        bool isShuttingDown = false;
        bool isPending = false;
        bool isPendingRecord = false;
        bool isPendingPlay = false;
        bool isPendingTranslate = false;
    };

    struct TransitionRecord {
        State fromState;
        Event event;
        State toState;
        uint64_t timestamp;
        bool succeeded;
    };

    struct Hooks {
        std::function<Result<void>(RecordingOptions)> prepareRecording;
        std::function<Result<void>()> activateRecording;
        std::function<Result<void>()> stopRecordingGraceful;
        std::function<void()> stopRecordingImmediate;
        std::function<bool()> isRecordingPrepared;
        std::function<bool()> isRecordingActive;

        std::function<Result<void>(TASProject *, PlaybackType)> preparePlayback;
        std::function<Result<void>()> activatePlayback;
        std::function<Result<void>(bool)> stopPlaybackGraceful;
        std::function<void()> stopPlaybackImmediate;
        std::function<void()> pausePlayback;
        std::function<void()> resumePlayback;
        std::function<bool()> isPlaybackPrepared;
        std::function<bool()> isPlaybackActiveOrPaused;
        std::function<TASProject *()> currentPlaybackProject;

        std::function<Result<void>(TASProject *)> prepareTranslation;
        std::function<Result<void>()> activateTranslation;
        std::function<Result<void>(bool)> stopTranslationGraceful;
        std::function<void()> stopTranslationImmediate;
        std::function<bool()> isTranslationPrepared;
        std::function<bool()> isTranslationActive;

        std::function<Result<void>(TASProject *)> startValidationForPlayback;
        std::function<Result<void>()> stopValidationGraceful;
        std::function<void()> stopValidationImmediate;
        std::function<bool()> isValidationActive;

        std::function<void(State, State)> onStateEntered;
    };

    RuntimeSession();
    RuntimeSession(EventBus *eventBus, Hooks hooks = {});

    RuntimeSession(const RuntimeSession &) = delete;
    RuntimeSession &operator=(const RuntimeSession &) = delete;

    Result<void> StartRecording(RecordingOptions options);
    Result<void> StartPlayback(TASProject *project, PlaybackType type, PlaybackOptions options);
    Result<void> StartTranslation(TASProject *project, TranslationOptions options);
    Result<void> Stop(StopOptions options);
    Result<void> Pause();
    Result<void> Resume();
    Result<void> Shutdown();

    Result<void> OnLevelLoadStart();
    Result<void> OnLevelStart();
    Result<void> OnLevelEnd(StopOptions options);
    Result<void> OnPlaybackCompleted(PlaybackType completedType);
    Result<void> OnTranslationCompleted();

    SessionSnapshot SnapshotState() const;
    SessionSnapshot Snapshot() const { return SnapshotState(); }

    State GetCurrentState() const { return m_State; }
    State GetPausedFromState() const { return m_PausedFromState; }
    PlaybackType GetPlaybackType() const;
    const std::vector<TransitionRecord> &GetTransitionHistory() const { return m_History; }
    void ClearHistory() { m_History.clear(); }

    bool IsIdle() const { return m_State == State::Idle; }
    bool IsRecording() const { return m_State == State::Recording; }
    bool IsPlaying() const { return m_State == State::PlayingScript || m_State == State::PlayingRecord; }
    bool IsTranslating() const { return m_State == State::Translating; }
    bool IsPaused() const { return m_State == State::Paused; }
    bool IsShuttingDown() const { return m_State == State::ShuttingDown; }
    bool IsPending() const;
    bool IsPendingRecord() const { return m_State == State::PendingRecord; }
    bool IsPendingPlay() const {
        return m_State == State::PendingScriptPlayback || m_State == State::PendingRecordPlayback;
    }
    bool IsPendingTranslate() const { return m_State == State::PendingTranslation; }

    static const char *StateToString(State state);
    static const char *EventToString(Event event);

private:
    Result<void> Transition(Event event, StopOptions stopOptions = StopOptions{false});
    Result<void> TransitionToState(State nextState, StopOptions stopOptions);
    State FindTransitionTarget(State state, Event event) const;
    void InitializeTransitionTable();
    void RecordTransition(State fromState, Event event, State toState, bool succeeded);
    void PublishStateChanged(State oldState, State newState);

    Result<void> EnterState(State state, State previousState);
    Result<void> ExitState(State state, State nextState, StopOptions stopOptions);

    EventBus *m_EventBus = nullptr;
    Hooks m_Hooks;
    State m_State = State::Idle;
    State m_PausedFromState = State::Idle;
    TASProject *m_Project = nullptr;
    PlaybackType m_RequestedPlaybackType = PlaybackType::None;
    bool m_RecordingValidation = false;
    bool m_PlaybackValidation = false;

    struct StateEventPair {
        State state;
        Event event;
        bool operator==(const StateEventPair &other) const {
            return state == other.state && event == other.event;
        }
    };

    struct StateEventHash {
        size_t operator()(const StateEventPair &pair) const {
            return std::hash<int>()(static_cast<int>(pair.state)) ^
                   (std::hash<int>()(static_cast<int>(pair.event)) << 1);
        }
    };

    std::unordered_map<StateEventPair, State, StateEventHash> m_TransitionTable;
    std::vector<TransitionRecord> m_History;
    static constexpr size_t MAX_HISTORY_SIZE = 100;
};
