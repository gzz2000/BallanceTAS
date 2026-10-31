#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "CounterStateTrace.h"

// Read-only, opt-in observation after LevelFinish. Game callbacks and data-array
// values are never changed. Keep sampling after TAS playback has stopped: that
// event precedes the game's time/life bonus conversion.
class ScoreArrayTrace {
public:
    ~ScoreArrayTrace() { Close(); CloseGameplay(); }
    void Reset() { m_Counter.Reset(); Close(); CloseGameplay(); m_GameplayInitialized = false; m_Process = 0; m_HaveLast = false; }
    template<class Loader> void Finish(Loader *bml) {
        Reset();
        const char *path = std::getenv("BALLANCE_SCORE_ARRAY_TRACE");
        if (!path || !*path) return;
        m_File = std::fopen(path, "wb");
        if (!m_File) { std::fprintf(stderr, "SCORE_ARRAY_TRACE open failed\n"); return; }
        const char *names[] = {"Energy", "CurrentLevel"};
        for (const char *name : names) {
            auto *array = bml->GetArrayByName(const_cast<char *>(name));
            if (!array) continue;
            for (int c = 0; c < array->GetColumnCount(); ++c)
                std::fprintf(m_File, "# schema,%s,%d,%d,%s\n", name, c,
                    int(array->GetColumnType(c)), array->GetColumnName(c) ? array->GetColumnName(c) : "");
        }
        std::fputs("phase,process,playing,paused,valid_mask,energy_points,energy_lives,level_points\n", m_File);
        Sample(bml, "finish", true);
    }
    template<class Loader> void Process(Loader *bml) {
        if (!m_File) return;
        ++m_Process; Sample(bml, "process", false);
    }
    template<class Loader> void End(Loader *bml) {
        if (m_File) Sample(bml, "end", true);
        Close(); CloseGameplay();
    }
    template<class Loader> void Gameplay(Loader *bml, std::uint64_t cursor) {
        m_Counter.Sample(bml, cursor);
        if (!m_GameplayInitialized) {
            m_GameplayInitialized = true;
            const char *path = std::getenv("BALLANCE_GAMEPLAY_SCORE_TRACE");
            if (path && *path) {
                m_Gameplay = std::fopen(path, "wb");
                if (m_Gameplay) std::fputs("cursor,valid_mask,points,lives,level_points\n", m_Gameplay);
                else std::fprintf(stderr, "GAMEPLAY_SCORE_TRACE open failed\n");
            }
        }
        if (!m_Gameplay) return;
        int values[3]{}; unsigned valid = 0;
        const char *names[] = {"Energy", "Energy", "CurrentLevel"};
        const int cols[] = {0, 1, 5};
        for (int i = 0; i < 3; ++i) {
            auto *a = bml->GetArrayByName(const_cast<char *>(names[i]));
            if (a && a->GetRowCount() > 0 && a->GetColumnCount() > cols[i] &&
                a->GetColumnType(cols[i]) == CKARRAYTYPE_INT &&
                a->GetElementValue(0, cols[i], &values[i])) valid |= 1u << i;
        }
        std::fprintf(m_Gameplay, "%llu,%u,%d,%d,%d\n", static_cast<unsigned long long>(cursor),
            valid, values[0], values[1], values[2]);
        if ((cursor & 255u) == 0) std::fflush(m_Gameplay);
    }
private:
    CounterStateTrace m_Counter;
    void CloseGameplay() { if (m_Gameplay) std::fclose(m_Gameplay); m_Gameplay = nullptr; }
    std::FILE *m_Gameplay = nullptr;
    bool m_GameplayInitialized = false;

    template<class Loader> void Sample(Loader *bml, const char *phase, bool force) {
        int values[6] = {int(bml->IsPlaying()), int(bml->IsPaused()), 0, 0, 0, 0};
        const char *names[] = {"Energy", "Energy", "CurrentLevel"};
        const int columns[] = {0, 1, 5};
        for (int i = 0; i < 3; ++i) {
            auto *a = bml->GetArrayByName(const_cast<char *>(names[i]));
            if (a && a->GetRowCount() > 0 && a->GetColumnCount() > columns[i] &&
                a->GetColumnType(columns[i]) == CKARRAYTYPE_INT &&
                a->GetElementValue(0, columns[i], &values[3 + i])) values[2] |= 1 << i;
        }
        if (force || !m_HaveLast || std::memcmp(values, m_Last, sizeof(values)) || m_Process % 60 == 0) {
            std::fprintf(m_File, "%s,%llu,%d,%d,%d,%d,%d,%d\n", phase,
                static_cast<unsigned long long>(m_Process), values[0], values[1], values[2],
                values[3], values[4], values[5]);
            std::fflush(m_File);
        }
        std::memcpy(m_Last, values, sizeof(values)); m_HaveLast = true;
    }
    void Close() { if (m_File) std::fclose(m_File); m_File = nullptr; }
    std::FILE *m_File = nullptr;
    std::uint64_t m_Process = 0;
    int m_Last[6]{};
    bool m_HaveLast = false;
};
