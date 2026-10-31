// Included by BallanceTAS.cpp so browser commands use the live mod lifecycle.
// Records stay in the existing parser/writer; no JS approximation of .tas data.
#include "RecordFileIO.h"
#include <cmath>
#include <emscripten.h>
namespace {
std::vector<RecordFrameData> webRecord, webUndo;
std::string webRecordError;
bool WebRecordMenuReady() {
    auto *engine = g_Mod ? g_Mod->GetEngine() : nullptr;
    if (!engine || !engine->IsIdle() || engine->GetGameInterface()->IsIngame()) {
        webRecordError = "请先停止 TAS 并退出关卡，在菜单中编辑录像。";
        return false;
    }
    return true;
}
bool WebRecordPath(const char *path) {
    if (!path)
        return false;
    const std::filesystem::path parsed(path);
    return parsed.parent_path() == "/user/ModLoader/TAS" && parsed.extension() == ".tas";
}
} // namespace
extern "C" {
EMSCRIPTEN_KEEPALIVE const char *BML_WebTASError() { return webRecordError.c_str(); }
EMSCRIPTEN_KEEPALIVE int BML_WebTASLoad(const char *path) {
    if (!WebRecordMenuReady())
        return -1;
    if (!WebRecordPath(path)) {
        webRecordError = "请选择 TAS 目录中的 .tas 文件。";
        return -1;
    }
    try {
        auto result = tas::record::ReadLegacyRecordFile(path);
        if (!result) {
            webRecordError = result.GetError().message;
            return -1;
        }
        auto frames = std::move(result.Unwrap());
        if (frames.empty() || frames.size() > 1000000) {
            webRecordError = "录像长度超出编辑范围。";
            return -1;
        }
        webRecord = std::move(frames);
        webUndo.clear();
        webRecordError.clear();
        return static_cast<int>(webRecord.size());
    } catch (const std::exception &e) {
        webRecordError = e.what();
        return -1;
    }
}
EMSCRIPTEN_KEEPALIVE int BML_WebTASKeys(int index) {
    return index >= 0 && size_t(index) < webRecord.size()
               ? webRecord[index].keyStates & tas::record::kRecordKeyMask
               : -1;
}
EMSCRIPTEN_KEEPALIVE double BML_WebTASDelta(int index) {
    return index >= 0 && size_t(index) < webRecord.size() ? webRecord[index].deltaTime : 0;
}
EMSCRIPTEN_KEEPALIVE int BML_WebTASSetRange(int first, int last, int keys, double delta) {
    if (!WebRecordMenuReady())
        return 0;
    if (first < 0 || last < first || size_t(last) >= webRecord.size() || keys < 0 || keys > 511 ||
        !std::isfinite(delta) || delta < 0 || delta > 1000) {
        webRecordError = "采样范围、按键或时间无效。";
        return 0;
    }
    webUndo = webRecord;
    for (int i = first; i <= last; ++i) {
        webRecord[i].keyStates = (webRecord[i].keyStates & ~tas::record::kRecordKeyMask) | keys;
        if (delta > 0)
            webRecord[i].deltaTime = static_cast<float>(delta);
    }
    return 1;
}
EMSCRIPTEN_KEEPALIVE int BML_WebTASUndo() {
    if (!WebRecordMenuReady() || webUndo.empty())
        return 0;
    webRecord.swap(webUndo);
    webUndo.clear();
    return 1;
}
EMSCRIPTEN_KEEPALIVE int BML_WebTASSaveCopy(const char *path) {
    if (!WebRecordMenuReady())
        return 0;
    if (!WebRecordPath(path) || webRecord.empty()) {
        webRecordError = "副本名称无效。";
        return 0;
    }
    try {
        if (std::filesystem::exists(path)) {
            webRecordError = "该文件已经存在，请换一个副本名称。";
            return 0;
        }
        auto result = tas::record::WriteLegacyRecordFile(path, webRecord);
        if (!result) {
            webRecordError = result.GetError().message;
            return 0;
        }
        return 1;
    } catch (const std::exception &e) {
        webRecordError = e.what();
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE int BML_WebTASPlay(const char *path, int level) {
    if (!WebRecordMenuReady())
        return 0;
    if (!WebRecordPath(path) || level < 1 || level > 13) {
        webRecordError = "录像或关卡无效。";
        return 0;
    }
    try {
        auto *engine = g_Mod->GetEngine();
        auto *projects = engine->GetServiceProvider().Resolve<ProjectManager>();
        auto *project = projects ? projects->ResolveLaunchTarget(path, webRecordError) : nullptr;
        if (!project || !project->IsValid() || !project->IsRecordProject())
            return 0;
        projects->SetCurrentProject(project);
        if (!engine->StartReplay()) {
            webRecordError = "无法准备录像。";
            return 0;
        }
        if (!engine->GetGameInterface()->LoadLevel(level, webRecordError)) {
            engine->StopReplay(false);
            return 0;
        }
        return 1;
    } catch (const std::exception &e) {
        webRecordError = e.what();
        return 0;
    }
}
}

namespace {
int webCheckpointRequest = 0, webCheckpointResult = 0;
bool webCheckpointValid = false, webCheckpointRecord = false;
size_t webCheckpointTick = 0;
TASProject *webCheckpointProject = nullptr;
} // namespace
static void InvalidateWebCheckpoint() {
    webCheckpointValid = false;
    webCheckpointRequest = 0;
    webCheckpointResult = -2;
    webCheckpointProject = nullptr;
    webRecordError = "关卡或 TAS 会话已结束，内存快照已丢弃。";
}
extern "C" EMSCRIPTEN_KEEPALIVE int BML_WebRequestCheckpoint(int action) {
    if ((action != 1 && action != 2) || webCheckpointRequest || !g_Mod || !g_Mod->GetEngine() ||
        !(g_Mod->GetBML()->IsPlaying() || g_Mod->GetBML()->IsPaused()))
        return 0;
    webCheckpointRequest = action;
    webCheckpointResult = 0;
    return 1;
}
extern "C" EMSCRIPTEN_KEEPALIVE int BML_WebCheckpointResult() { return webCheckpointResult; }
static void RunWebCheckpointRequest(BallanceTAS *mod) {
    if (!webCheckpointRequest)
        return;
    const int action = webCheckpointRequest;
    webCheckpointRequest = 0;
    webCheckpointResult = -1;
    auto *engine = mod->GetEngine();
    auto *playback = engine->GetServiceProvider().Resolve<PlaybackService>();
    const bool record =
        playback && playback->IsPlaying() && playback->GetPlaybackType() == PlaybackType::Record;
    // Script VMs need their own resumable continuation, beyond a record cursor.
    if (!engine->IsIdle() && !record) {
        webRecordError = "当前 TAS 操作不支持此练习快照。";
        return;
    }
    if (action == 1) {
        if (!BML_PracticeCaptureState()) {
            webRecordError = "无法保存当前关卡状态。";
            return;
        }
        webCheckpointValid = true;
        webCheckpointRecord = record;
        webCheckpointTick = record ? playback->GetCurrentTick() : 0;
        webCheckpointProject = record ? playback->GetCurrentProject() : nullptr;
    } else {
        if (!webCheckpointValid || record != webCheckpointRecord ||
            (record && playback->GetCurrentProject() != webCheckpointProject)) {
            webRecordError = "此关卡或此 TAS 会话没有可恢复的快照。";
            return;
        }
        if (!BML_PracticeRestoreState() ||
            (record && !playback->RewindRecordTo(webCheckpointTick))) {
            webRecordError = "状态恢复失败，请查看游戏日志。";
            return;
        }
    }
    webCheckpointResult = action;
    Log::Info("Browser checkpoint action=%d record=%d tick=%zu.", action, int(record),
              webCheckpointTick);
}
