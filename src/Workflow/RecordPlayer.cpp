#include "RecordPlayer.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <limits>

#include "Logger.h"
#include "TASEngine.h"
#include "TASProject.h"
#include "GameInterface.h"

constexpr float kDefaultFrameDeltaTimeMs = 1000.0f / 132.0f;

static void EnsureFrameSentinel(std::vector<RecordFrameData> &frames, size_t totalFrames) {
    if (frames.size() < totalFrames) {
        frames.resize(totalFrames);
    }

    frames.resize(totalFrames + 1);
    frames[totalFrames] = RecordFrameData();
}

static void ClampPlaybackCursor(size_t &currentFrame, size_t totalFrames) {
    if (currentFrame > totalFrames) {
        currentFrame = totalFrames;
    }
}

static bool CanUseLegacyPackSize(size_t byteCount) {
    return byteCount <= static_cast<size_t>((std::numeric_limits<int>::max)())
        && byteCount <= static_cast<size_t>((std::numeric_limits<uint32_t>::max)());
}

bool RecordPlayer::Fail(const char *format, ...) const {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    m_LastError = buffer;
    Log::Warn("%s", buffer);
    return false;
}

RecordPlayer::RecordPlayer(TASEngine *engine) : m_Engine(engine) {
    if (!m_Engine) {
        throw std::runtime_error("RecordPlayer requires a valid TASEngine instance.");
    }
}

bool RecordPlayer::LoadAndPlay(const TASProject *project) {
    if (!project || !project->IsRecordProject() || !project->CanPlayRecord()) {
        return Fail("Invalid record project provided: %s", project ? project->GetValidationMessage().c_str() : "null project");
    }

    // Stop any current playback
    Stop();

    std::string recordPath = project->GetRecordFilePath();
    if (!LoadRecord(recordPath)) {
        return Fail("Failed to load record: %s", recordPath.c_str());
    }

    // Acquire remapped keys from game interface
    auto *gameInterface = m_Engine->GetGameInterface();
    if (gameInterface) {
        m_KeyUp = gameInterface->RemapKey(CKKEY_UP);
        m_KeyDown = gameInterface->RemapKey(CKKEY_DOWN);
        m_KeyLeft = gameInterface->RemapKey(CKKEY_LEFT);
        m_KeyRight = gameInterface->RemapKey(CKKEY_RIGHT);
        m_KeyShift = gameInterface->RemapKey(CKKEY_LSHIFT);
        m_KeySpace = gameInterface->RemapKey(CKKEY_SPACE);
    }

    m_IsPlaying = true;

    NotifyStatusChange(true);

    Log::Info("Record '%s' loaded and playback started (%zu frames).",
                                project->GetName().c_str(), m_TotalFrames);
    return true;
}

void RecordPlayer::Stop() {
    if (!m_IsPlaying) return;

    m_IsPlaying = false;
    m_IsPaused = false;
    m_CurrentFrame = 0;
    m_TotalFrames = 0;
    m_Frames.clear();
    m_Frames.shrink_to_fit();

    Log::Info("Record playback stopped.");
}

void RecordPlayer::Pause() {
    if (!m_IsPlaying || m_IsPaused) {
        return;
    }

    m_IsPaused = true;
    Log::Info("Record playback paused at frame %zu.", m_CurrentFrame);
}

void RecordPlayer::Resume() {
    if (!m_IsPlaying || !m_IsPaused) {
        return;
    }

    m_IsPaused = false;
    Log::Info("Record playback resumed from frame %zu.", m_CurrentFrame);
}

bool RecordPlayer::Seek(size_t frame) {
    if (!m_IsPlaying) {
        return Fail("Cannot seek: no record is playing.");
    }

    if (frame >= m_TotalFrames) {
        return Fail("Seek frame %zu is out of bounds (total: %zu).", frame, m_TotalFrames);
    }

    m_CurrentFrame = frame;
    Log::Info("Seeked to frame %zu.", frame);
    return true;
}

bool RecordPlayer::LoadAndPlay(const std::string &recordPath) {
    // Stop any current playback
    Stop();

    if (!LoadRecord(recordPath)) {
        return Fail("Failed to load record: %s", recordPath.c_str());
    }

    // Acquire remapped keys from game interface
    auto *gameInterface = m_Engine->GetGameInterface();
    if (gameInterface) {
        m_KeyUp = gameInterface->RemapKey(CKKEY_UP);
        m_KeyDown = gameInterface->RemapKey(CKKEY_DOWN);
        m_KeyLeft = gameInterface->RemapKey(CKKEY_LEFT);
        m_KeyRight = gameInterface->RemapKey(CKKEY_RIGHT);
        m_KeyShift = gameInterface->RemapKey(CKKEY_LSHIFT);
        m_KeySpace = gameInterface->RemapKey(CKKEY_SPACE);
    }

    m_IsPlaying = true;
    m_IsPaused = false;
    m_CurrentFrame = 0;

    NotifyStatusChange(true);

    Log::Info("Record loaded and playback started (%zu frames): %s",
                                m_TotalFrames, recordPath.c_str());
    return true;
}

void RecordPlayer::Tick(size_t currentTick, unsigned char *keyboardState) {
    if (!m_IsPlaying) {
        return;
    }

    // If paused, don't advance or apply input
    if (m_IsPaused) {
        return;
    }

    // Use internal frame tracking
    if (m_CurrentFrame >= m_TotalFrames) {
        Log::Info("Record playback completed naturally.");
        Stop();
        NotifyStatusChange(false);
        return;
    }

    if (m_Frames.size() <= m_CurrentFrame) {
        Log::Error("Record playback frame storage is corrupt (frame=%zu total=%zu size=%zu).",
                   m_CurrentFrame, m_TotalFrames, m_Frames.size());
        Stop();
        NotifyStatusChange(false);
        return;
    }

    ApplyCurrentFrame(keyboardState);

    // Advance to next frame
    m_CurrentFrame++;
}

void RecordPlayer::ApplyCurrentFrame(unsigned char *keyboardState) const {
    if (!m_IsPlaying || m_IsPaused || m_CurrentFrame >= m_TotalFrames ||
        m_Frames.size() <= m_CurrentFrame) {
        return;
    }

    const RecordFrameData idleFrame{};
    const RecordFrameData &nextFrame =
        (m_CurrentFrame + 1 < m_TotalFrames) ? m_Frames[m_CurrentFrame + 1] : idleFrame;
    ApplyFrameInput(m_Frames[m_CurrentFrame], nextFrame, keyboardState);
}

bool RecordPlayer::RestoreConsumedFrameInput(unsigned char *keyboardState) const {
    if (!m_IsPlaying || !keyboardState || m_CurrentFrame >= m_TotalFrames ||
        m_CurrentFrame >= m_Frames.size())
        return false;
    // Tick applies frame N-1 and then advances the cursor to N. Reproduce both
    // pressed and legacy release bits, leaving the next sample unconsumed.
    const RecordFrameData idleFrame{};
    const RecordFrameData &consumed = m_CurrentFrame
        ? m_Frames[m_CurrentFrame - 1] : idleFrame;
    ApplyFrameInput(consumed, m_Frames[m_CurrentFrame], keyboardState);
    return true;
}

float RecordPlayer::GetFrameDeltaTime(size_t currentTick) const {
    if (!m_IsPlaying || m_CurrentFrame >= m_TotalFrames) {
        return kDefaultFrameDeltaTimeMs;
    }
    return m_Frames[m_CurrentFrame].deltaTime;
}

bool RecordPlayer::LoadRecord(const std::string &recordPath) {
    try {
        Log::Info("Loading TAS record: %s", recordPath.c_str());

        // --- 1. Read the entire file ---
        std::ifstream file(recordPath, std::ios::binary);
        if (!file) {
            return Fail("Could not open record file: %s", recordPath.c_str());
        }

        // --- 2. Read the 4-byte header for uncompressed size (legacy format) ---
        uint32_t uncompressedSize = 0;
        file.read(reinterpret_cast<char *>(&uncompressedSize), sizeof(uncompressedSize));
        if (file.gcount() != sizeof(uncompressedSize)) {
            return Fail("Failed to read uncompressed size header from file.");
        }

        if (uncompressedSize == 0) {
            m_TotalFrames = 0;
            m_Frames.clear();
            return Fail("Record has no frames.");
        }

        // The uncompressed data must be a multiple of the FrameData size.
        if (uncompressedSize % sizeof(RecordFrameData) != 0) {
            return Fail( "Uncompressed size is not a multiple of FrameData size. File may be corrupt.");
        }

        if (!CanUseLegacyPackSize(uncompressedSize)) {
            return Fail("Record payload is too large for legacy packing format: %u bytes.", uncompressedSize);
        }

        // --- 3. Read the compressed payload ---
        file.seekg(0, std::ios::end);
        std::streampos fileSize = file.tellg();
        if (fileSize < static_cast<std::streampos>(sizeof(uncompressedSize))) {
            return Fail("Record file is truncated or has an invalid size.");
        }

        file.seekg(sizeof(uncompressedSize), std::ios::beg);

        size_t compressedSize = static_cast<size_t>(fileSize) - sizeof(uncompressedSize);
        if (compressedSize == 0) {
            return Fail("Record file is missing the compressed payload.");
        }

        std::vector<char> compressedData(compressedSize);
        file.read(compressedData.data(), compressedSize);
        if (static_cast<size_t>(file.gcount()) != compressedSize) {
            return Fail("Failed to read compressed data payload.");
        }
        file.close();

        // --- 4. Decompress using CKUnPackData function ---
        char *uncompressedData = CKUnPackData(static_cast<int>(uncompressedSize), compressedData.data(),
                                              compressedSize);
        if (!uncompressedData) {
            return Fail("Failed to decompress TAS record data using CKUnPackData.");
        }

        // --- 5. Copy the decompressed data to our frame vector ---
        size_t frameCount = uncompressedSize / sizeof(RecordFrameData);
        m_TotalFrames = frameCount;
        EnsureFrameSentinel(m_Frames, m_TotalFrames);
        memcpy(m_Frames.data(), uncompressedData, uncompressedSize);
        m_Frames[m_TotalFrames] = RecordFrameData();
        m_CurrentFrame = 0;
        m_IsModified = false;
        ResetEditState();
        LoadAnnotations(GetAnnotationsPath(recordPath));

        // Clean up the decompressed data
        CKDeletePointer(uncompressedData);

        Log::Info("Record loaded successfully: %zu frames", m_TotalFrames);
        return true;
    } catch (const std::exception &e) {
        return Fail("Exception loading record: %s", e.what());
    }
}

void RecordPlayer::ApplyFrameInput(const RecordFrameData &currentFrame,
                                   const RecordFrameData &nextFrame,
                                   unsigned char *keyboardState) const {
    if (!keyboardState) {
        Log::Error("Keyboard state buffer is null. Cannot apply input.");
        return;
    }

    const RecordKeyState &current = currentFrame.keyState;
    const RecordKeyState &next = nextFrame.keyState;

    keyboardState[m_KeyUp] = ConvertKeyState(current.key_up, next.key_up);
    keyboardState[m_KeyDown] = ConvertKeyState(current.key_down, next.key_down);
    keyboardState[m_KeyLeft] = ConvertKeyState(current.key_left, next.key_left);
    keyboardState[m_KeyRight] = ConvertKeyState(current.key_right, next.key_right);
    keyboardState[CKKEY_Q] = ConvertKeyState(current.key_q, next.key_q);
    keyboardState[m_KeyShift] = ConvertKeyState(current.key_shift, next.key_shift);
    keyboardState[m_KeySpace] = ConvertKeyState(current.key_space, next.key_space);
    keyboardState[CKKEY_ESCAPE] = ConvertKeyState(current.key_esc, next.key_esc);
}

unsigned char RecordPlayer::ConvertKeyState(bool current, bool next) {
    unsigned char state = KS_IDLE;
    if (current) {
        state |= KS_PRESSED;
        if (!next) {
            state |= KS_RELEASED;
        }
    }
    return state;
}

// ===================================================================
// Frame-Level Input Query & Modification Implementation
// ===================================================================

bool RecordPlayer::GetFrameInput(size_t frame, RecordFrameData *outData) const {
    if (!outData) {
        return Fail("GetFrameInput: outData pointer is null.");
    }

    if (frame >= m_TotalFrames) {
        return Fail("GetFrameInput: frame %zu is out of bounds (total: %zu).", frame, m_TotalFrames);
    }

    *outData = m_Frames[frame];
    return true;
}

bool RecordPlayer::SetFrameInput(size_t frame, const RecordFrameData &inputData) {
    if (frame >= m_TotalFrames) {
        return Fail("SetFrameInput: frame %zu is out of bounds (total: %zu).", frame, m_TotalFrames);
    }

    PushUndoAction(EditActionType::ModifyFrame, "Set frame " + std::to_string(frame));
    m_Frames[frame] = inputData;
    m_IsModified = true;
    return true;
}

bool RecordPlayer::SetFrameKey(size_t frame, const std::string &key, bool pressed) {
    if (frame >= m_TotalFrames) {
        return Fail("SetFrameKey: frame %zu is out of bounds (total: %zu).", frame, m_TotalFrames);
    }

    RecordKeyState keyState = m_Frames[frame].keyState;
    if (!SetKeyStateBit(keyState, key, pressed)) {
        return Fail("SetFrameKey: invalid key name '%s'.", key.c_str());
    }

    PushUndoAction(EditActionType::SetFrameKey, "Set " + key + " at frame " + std::to_string(frame));
    m_Frames[frame].keyState = keyState;

    m_IsModified = true;
    return true;
}

float RecordPlayer::GetFrameDeltaTimeByFrame(size_t frame) const {
    if (frame >= m_TotalFrames) {
        return 0.0f;
    }
    return m_Frames[frame].deltaTime;
}

bool RecordPlayer::SetFrameDeltaTime(size_t frame, float deltaTime) {
    if (frame >= m_TotalFrames) {
        return Fail("SetFrameDeltaTime: frame %zu is out of bounds (total: %zu).", frame, m_TotalFrames);
    }

    if (deltaTime <= 0.0f) {
        return Fail("SetFrameDeltaTime: deltaTime must be positive.");
    }

    PushUndoAction(EditActionType::SetDeltaTime, "Set delta time at frame " + std::to_string(frame));
    m_Frames[frame].deltaTime = deltaTime;
    m_IsModified = true;
    return true;
}

// ===================================================================
// Bulk Frame Operations Implementation
// ===================================================================

bool RecordPlayer::InsertFrames(size_t startFrame, size_t count) {
    if (count == 0) {
        return true;
    }

    if (startFrame > m_TotalFrames) {
        return Fail("InsertFrames: startFrame %zu is out of bounds (total: %zu).", startFrame, m_TotalFrames);
    }

    // Create blank frames with default delta time
    RecordFrameData blankFrame;
    blankFrame.deltaTime = m_TotalFrames > 0 ? m_Frames[0].deltaTime : kDefaultFrameDeltaTimeMs;
    blankFrame.keyStates = 0;

    PushUndoAction(EditActionType::InsertFrames, "Insert " + std::to_string(count) + " frames");
    // Insert blank frames at the specified position
    m_Frames.insert(m_Frames.begin() + startFrame, count, blankFrame);
    m_TotalFrames += count;
    EnsureFrameSentinel(m_Frames, m_TotalFrames);
    ClampPlaybackCursor(m_CurrentFrame, m_TotalFrames);
    m_IsModified = true;

    Log::Info("Inserted %zu blank frames at position %zu.", count, startFrame);
    return true;
}

bool RecordPlayer::DeleteFrames(size_t startFrame, size_t count) {
    if (count == 0) {
        return true;
    }

    if (startFrame >= m_TotalFrames) {
        return Fail("DeleteFrames: startFrame %zu is out of bounds (total: %zu).", startFrame, m_TotalFrames);
    }

    size_t actualCount = (std::min)(count, m_TotalFrames - startFrame);
    PushUndoAction(EditActionType::DeleteFrames, "Delete " + std::to_string(actualCount) + " frames");
    m_Frames.erase(m_Frames.begin() + startFrame, m_Frames.begin() + startFrame + actualCount);
    m_TotalFrames -= actualCount;
    EnsureFrameSentinel(m_Frames, m_TotalFrames);
    ClampPlaybackCursor(m_CurrentFrame, m_TotalFrames);
    m_IsModified = true;

    Log::Info("Deleted %zu frames starting at position %zu.", actualCount, startFrame);
    return true;
}

bool RecordPlayer::CopyFrames(size_t srcStart, size_t destStart, size_t count) {
    if (count == 0) {
        return true;
    }

    if (srcStart >= m_TotalFrames) {
        return Fail("CopyFrames: srcStart %zu is out of bounds (total: %zu).", srcStart, m_TotalFrames);
    }

    if (destStart > m_TotalFrames) {
        return Fail("CopyFrames: destStart %zu is out of bounds (total: %zu).", destStart, m_TotalFrames);
    }

    size_t actualCount = (std::min)(count, m_TotalFrames - srcStart);

    // Copy the frame range
    std::vector<RecordFrameData> copiedFrames(m_Frames.begin() + srcStart, m_Frames.begin() + srcStart + actualCount);

    PushUndoAction(EditActionType::CopyFrames, "Copy " + std::to_string(actualCount) + " frames");
    // Insert/overwrite at destination
    if (destStart + actualCount > m_TotalFrames) {
        // Extend the vector if necessary
        m_TotalFrames = destStart + actualCount;
        EnsureFrameSentinel(m_Frames, m_TotalFrames);
    }

    std::copy(copiedFrames.begin(), copiedFrames.end(), m_Frames.begin() + destStart);
    EnsureFrameSentinel(m_Frames, m_TotalFrames);
    ClampPlaybackCursor(m_CurrentFrame, m_TotalFrames);
    m_IsModified = true;

    Log::Info("Copied %zu frames from %zu to %zu.", actualCount, srcStart, destStart);
    return true;
}

bool RecordPlayer::DuplicateFrame(size_t frame, size_t count) {
    if (frame >= m_TotalFrames) {
        return Fail("DuplicateFrame: frame %zu is out of bounds (total: %zu).", frame, m_TotalFrames);
    }

    if (count == 0) {
        return true;
    }

    PushUndoAction(EditActionType::DuplicateFrame, "Duplicate frame " + std::to_string(frame));
    RecordFrameData frameData = m_Frames[frame];
    m_Frames.insert(m_Frames.begin() + frame + 1, count, frameData);
    m_TotalFrames += count;
    EnsureFrameSentinel(m_Frames, m_TotalFrames);
    ClampPlaybackCursor(m_CurrentFrame, m_TotalFrames);
    m_IsModified = true;

    Log::Info("Duplicated frame %zu %zu times.", frame, count);
    return true;
}

// ===================================================================
// Advanced Playback Control Implementation
// ===================================================================

bool RecordPlayer::SeekRelative(int offset) {
    if (!m_IsPlaying) {
        return Fail("SeekRelative: no record is playing.");
    }

    int newFrame = static_cast<int>(m_CurrentFrame) + offset;
    if (newFrame < 0 || newFrame >= static_cast<int>(m_TotalFrames)) {
        return Fail("SeekRelative: offset %d would result in frame %d (out of bounds).", offset, newFrame);
    }

    m_CurrentFrame = static_cast<size_t>(newFrame);
    Log::Info("Seeked relatively by %d to frame %zu.", offset, m_CurrentFrame);
    return true;
}

float RecordPlayer::GetProgress() const {
    if (m_TotalFrames == 0) {
        return 0.0f;
    }
    return static_cast<float>(m_CurrentFrame) / static_cast<float>(m_TotalFrames);
}

// ===================================================================
// Input Display & Analysis Implementation
// ===================================================================

std::string RecordPlayer::GetInputString(size_t frame) const {
    if (frame >= m_TotalFrames) {
        return "";
    }

    const RecordKeyState &keyState = m_Frames[frame].keyState;
    std::string result;

    // Build a string representation of pressed keys
    if (keyState.key_up) result += "Up ";
    if (keyState.key_down) result += "Down ";
    if (keyState.key_left) result += "Left ";
    if (keyState.key_right) result += "Right ";
    if (keyState.key_shift) result += "Shift ";
    if (keyState.key_space) result += "Space ";
    if (keyState.key_q) result += "Q ";
    if (keyState.key_esc) result += "Esc ";
    if (keyState.key_enter) result += "Enter ";

    // Remove trailing space
    if (!result.empty()) {
        result.pop_back();
    }

    return result;
}

std::vector<std::string> RecordPlayer::GetPressedKeys(size_t frame) const {
    std::vector<std::string> keys;
    if (frame >= m_TotalFrames) {
        return keys;
    }

    const RecordKeyState &keyState = m_Frames[frame].keyState;

    if (keyState.key_up) keys.push_back("up");
    if (keyState.key_down) keys.push_back("down");
    if (keyState.key_left) keys.push_back("left");
    if (keyState.key_right) keys.push_back("right");
    if (keyState.key_shift) keys.push_back("shift");
    if (keyState.key_space) keys.push_back("space");
    if (keyState.key_q) keys.push_back("q");
    if (keyState.key_esc) keys.push_back("esc");
    if (keyState.key_enter) keys.push_back("enter");

    return keys;
}

int RecordPlayer::FindInputChange(size_t startFrame, const std::string &key) const {
    if (startFrame >= m_TotalFrames) {
        return -1;
    }

    bool currentState = GetKeyStateBit(m_Frames[startFrame].keyState, key);

    for (size_t i = startFrame + 1; i < m_TotalFrames; i++) {
        bool newState = GetKeyStateBit(m_Frames[i].keyState, key);
        if (newState != currentState) {
            return static_cast<int>(i);
        }
    }

    return -1; // No change found
}

// ===================================================================
// Validation & Comparison Implementation
// ===================================================================

bool RecordPlayer::CompareFrames(size_t frame1, size_t frame2) const {
    if (frame1 >= m_TotalFrames || frame2 >= m_TotalFrames) {
        return false;
    }

    const RecordFrameData &f1 = m_Frames[frame1];
    const RecordFrameData &f2 = m_Frames[frame2];

    return f1.deltaTime == f2.deltaTime && f1.keyStates == f2.keyStates;
}

bool RecordPlayer::Validate() const {
    if (m_TotalFrames == 0) {
        Log::Warn("Validate: record is empty.");
        return true; // Empty records are valid
    }

    // Check for valid delta times
    for (size_t i = 0; i < m_TotalFrames; i++) {
        if (m_Frames[i].deltaTime <= 0.0f) {
            return Fail("Validate: frame %zu has invalid delta time %.3f.", i, m_Frames[i].deltaTime);
        }
    }

    Log::Info("Validate: record is valid (%zu frames).", m_TotalFrames);
    return true;
}

// ===================================================================
// Save & Export Operations Implementation
// ===================================================================

bool RecordPlayer::Save(const std::string &path) {
    if (!SaveFrames(path)) {
        return false;
    }

    const std::string annotationsPath = GetAnnotationsPath(path);
    if (HasAnnotations()) {
        return SaveAnnotations(annotationsPath);
    }
    // Don't let an older sidecar resurrect annotations that were removed.
    std::remove(annotationsPath.c_str());
    return true;
}

bool RecordPlayer::SaveFrames(const std::string &path) {
    try {
        Log::Info("Saving record to: %s", path.c_str());

        if (m_TotalFrames == 0) {
            std::ofstream file(path, std::ios::binary);
            if (!file) {
                return Fail("Failed to open file for writing: %s", path.c_str());
            }

            const uint32_t header = 0;
            file.write(reinterpret_cast<const char *>(&header), sizeof(header));
            file.close();

            m_IsModified = false;
            Log::Info("Record saved successfully: 0 frames.");
            return true;
        }

        // Prepare uncompressed data
        size_t uncompressedSize = m_TotalFrames * sizeof(RecordFrameData);
        if (!CanUseLegacyPackSize(uncompressedSize)) {
            return Fail("Record is too large to save in legacy format: %zu bytes.", uncompressedSize);
        }

        EnsureFrameSentinel(m_Frames, m_TotalFrames);
        const char *uncompressedData = reinterpret_cast<const char *>(m_Frames.data());

        // Compress data using CKPackData
        int compressedSize = 0;
        char *compressedData = CKPackData(const_cast<char *>(uncompressedData), static_cast<int>(uncompressedSize),
                                          compressedSize, 9);

        if (!compressedData || compressedSize <= 0) {
            return Fail("Failed to compress record data.");
        }

        // Write to file
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            CKDeletePointer(compressedData);
            return Fail("Failed to open file for writing: %s", path.c_str());
        }

        // Write uncompressed size header
        uint32_t header = static_cast<uint32_t>(uncompressedSize);
        file.write(reinterpret_cast<const char *>(&header), sizeof(header));

        // Write compressed data
        file.write(compressedData, compressedSize);
        file.close();

        // Clean up compressed data
        CKDeletePointer(compressedData);

        m_IsModified = false;
        Log::Info("Record saved successfully: %zu frames.", m_TotalFrames);
        return true;
    } catch (const std::exception &e) {
        return Fail("Exception saving record: %s", e.what());
    }
}

bool RecordPlayer::ExportInputs(const std::string &path, const std::string &format) {
    try {
        Log::Info("Exporting inputs to: %s (format: %s)", path.c_str(), format.c_str());

        std::ofstream file(path);
        if (!file) {
            return Fail("Failed to open file for writing: %s", path.c_str());
        }

        if (format == "txt") {
            // Plain text format
            file << "Frame,DeltaTime,Keys\n";
            for (size_t i = 0; i < m_TotalFrames; i++) {
                file << i << "," << m_Frames[i].deltaTime << "," << GetInputString(i) << "\n";
            }
        } else if (format == "json") {
            // JSON format
            file << "{\n";
            file << "  \"totalFrames\": " << m_TotalFrames << ",\n";
            file << "  \"frames\": [\n";
            for (size_t i = 0; i < m_TotalFrames; i++) {
                file << "    {\n";
                file << "      \"frame\": " << i << ",\n";
                file << "      \"deltaTime\": " << m_Frames[i].deltaTime << ",\n";
                file << "      \"keys\": [";

                auto keys = GetPressedKeys(i);
                for (size_t j = 0; j < keys.size(); j++) {
                    file << "\"" << keys[j] << "\"";
                    if (j < keys.size() - 1) file << ", ";
                }

                file << "]\n";
                file << "    }";
                if (i < m_TotalFrames - 1) file << ",";
                file << "\n";
            }
            file << "  ]\n";
            file << "}\n";
        } else {
            return Fail("ExportInputs: unsupported format '%s'.", format.c_str());
        }

        file.close();
        Log::Info("Inputs exported successfully.");
        return true;
    } catch (const std::exception &e) {
        return Fail("Exception exporting inputs: %s", e.what());
    }
}

// ===================================================================
// Helper Methods Implementation
// ===================================================================

bool RecordPlayer::GetKeyStateBit(const RecordKeyState &keyState, const std::string &keyName) {
    if (keyName == "up") return keyState.key_up;
    if (keyName == "down") return keyState.key_down;
    if (keyName == "left") return keyState.key_left;
    if (keyName == "right") return keyState.key_right;
    if (keyName == "shift") return keyState.key_shift;
    if (keyName == "space") return keyState.key_space;
    if (keyName == "q") return keyState.key_q;
    if (keyName == "esc") return keyState.key_esc;
    if (keyName == "enter") return keyState.key_enter;
    return false;
}

bool RecordPlayer::SetKeyStateBit(RecordKeyState &keyState, const std::string &keyName, bool pressed) {
    if (keyName == "up") {
        keyState.key_up = pressed;
        return true;
    }
    if (keyName == "down") {
        keyState.key_down = pressed;
        return true;
    }
    if (keyName == "left") {
        keyState.key_left = pressed;
        return true;
    }
    if (keyName == "right") {
        keyState.key_right = pressed;
        return true;
    }
    if (keyName == "shift") {
        keyState.key_shift = pressed;
        return true;
    }
    if (keyName == "space") {
        keyState.key_space = pressed;
        return true;
    }
    if (keyName == "q") {
        keyState.key_q = pressed;
        return true;
    }
    if (keyName == "esc") {
        keyState.key_esc = pressed;
        return true;
    }
    if (keyName == "enter") {
        keyState.key_enter = pressed;
        return true;
    }
    return false;
}

// ===================================================================
// Enhanced Features Implementation
// ===================================================================

// --- Section Management ---

bool RecordPlayer::AddSection(size_t startFrame, size_t endFrame, const std::string &name,
                              const std::string &description, const std::string &color) {
    if (startFrame > endFrame || endFrame >= m_TotalFrames) {
        return Fail("Invalid section range: %zu-%zu", startFrame, endFrame);
    }

    if (m_Sections.find(name) != m_Sections.end()) {
        return Fail("Section '%s' already exists.", name.c_str());
    }

    RecordSection section(name, startFrame, endFrame);
    section.description = description;
    section.color = color;

    PushUndoAction(EditActionType::AddSection, "Add section: " + name);
    m_Sections[name] = section;

    Log::Info("Added section '%s' [%zu-%zu]", name.c_str(), startFrame, endFrame);
    return true;
}

bool RecordPlayer::RemoveSection(const std::string &name) {
    auto it = m_Sections.find(name);
    if (it == m_Sections.end()) {
        return Fail("no section named '%s'", name.c_str());
    }

    PushUndoAction(EditActionType::RemoveSection, "Remove section: " + name);
    m_Sections.erase(it);

    Log::Info("Removed section '%s'", name.c_str());
    return true;
}

const RecordSection *RecordPlayer::GetSection(const std::string &name) const {
    auto it = m_Sections.find(name);
    return (it != m_Sections.end()) ? &it->second : nullptr;
}

std::vector<RecordSection> RecordPlayer::GetAllSections() const {
    std::vector<RecordSection> result;
    result.reserve(m_Sections.size());
    for (const auto &pair : m_Sections) {
        result.push_back(pair.second);
    }
    return result;
}

const RecordSection *RecordPlayer::GetSectionAtFrame(size_t frame) const {
    for (const auto &pair : m_Sections) {
        const auto &section = pair.second;
        if (frame >= section.startFrame && frame <= section.endFrame) {
            return &section;
        }
    }
    return nullptr;
}

bool RecordPlayer::RenameSection(const std::string &oldName, const std::string &newName) {
    auto it = m_Sections.find(oldName);
    if (it == m_Sections.end()) {
        return Fail("no section named '%s'", oldName.c_str());
    }

    if (m_Sections.find(newName) != m_Sections.end()) {
        return Fail("Section '%s' already exists.", newName.c_str());
    }

    PushUndoAction(EditActionType::AddSection, "Rename section: " + oldName);
    RecordSection section = it->second;
    section.name = newName;
    m_Sections.erase(it);
    m_Sections[newName] = section;

    return true;
}

bool RecordPlayer::MoveSection(const std::string &name, size_t newStartFrame) {
    auto it = m_Sections.find(name);
    if (it == m_Sections.end()) {
        return Fail("no section named '%s'", name.c_str());
    }

    size_t length = it->second.endFrame - it->second.startFrame;
    size_t newEndFrame = newStartFrame + length;

    if (newEndFrame >= m_TotalFrames) {
        return Fail("the moved section would end past the last frame");
    }

    PushUndoAction(EditActionType::AddSection, "Move section: " + name);
    it->second.startFrame = newStartFrame;
    it->second.endFrame = newEndFrame;

    return true;
}

bool RecordPlayer::ResizeSection(const std::string &name, size_t newStartFrame, size_t newEndFrame) {
    auto it = m_Sections.find(name);
    if (it == m_Sections.end()) {
        return Fail("no section named '%s'", name.c_str());
    }

    if (newStartFrame > newEndFrame || newEndFrame >= m_TotalFrames) {
        return Fail("invalid section range %zu-%zu", newStartFrame, newEndFrame);
    }

    PushUndoAction(EditActionType::AddSection, "Resize section: " + name);
    it->second.startFrame = newStartFrame;
    it->second.endFrame = newEndFrame;

    return true;
}

// --- Marker Management ---

bool RecordPlayer::AddMarker(size_t frame, const std::string &name, MarkerType type,
                             const std::string &description, const std::string &color) {
    if (frame >= m_TotalFrames) {
        return Fail("frame %zu is out of range (%zu frames)", frame, m_TotalFrames);
    }

    if (m_Markers.find(name) != m_Markers.end()) {
        return Fail("Marker '%s' already exists.", name.c_str());
    }

    RecordMarker marker(name, frame, type);
    marker.description = description;
    marker.color = color;

    PushUndoAction(EditActionType::AddMarker, "Add marker: " + name);
    m_Markers[name] = marker;

    Log::Info("Added marker '%s' at frame %zu", name.c_str(), frame);
    return true;
}

bool RecordPlayer::RemoveMarker(const std::string &name) {
    auto it = m_Markers.find(name);
    if (it == m_Markers.end()) {
        return Fail("no marker named '%s'", name.c_str());
    }

    PushUndoAction(EditActionType::RemoveMarker, "Remove marker: " + name);
    m_Markers.erase(it);

    Log::Info("Removed marker '%s'", name.c_str());
    return true;
}

const RecordMarker *RecordPlayer::GetMarker(const std::string &name) const {
    auto it = m_Markers.find(name);
    return (it != m_Markers.end()) ? &it->second : nullptr;
}

std::vector<RecordMarker> RecordPlayer::GetAllMarkers() const {
    std::vector<RecordMarker> result;
    result.reserve(m_Markers.size());
    for (const auto &pair : m_Markers) {
        result.push_back(pair.second);
    }
    return result;
}

std::vector<RecordMarker> RecordPlayer::GetMarkersAtFrame(size_t frame) const {
    std::vector<RecordMarker> result;
    for (const auto &pair : m_Markers) {
        if (pair.second.frame == frame) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::vector<RecordMarker> RecordPlayer::GetMarkersInRange(size_t startFrame, size_t endFrame) const {
    std::vector<RecordMarker> result;
    for (const auto &pair : m_Markers) {
        if (pair.second.frame >= startFrame && pair.second.frame <= endFrame) {
            result.push_back(pair.second);
        }
    }
    return result;
}

bool RecordPlayer::RenameMarker(const std::string &oldName, const std::string &newName) {
    auto it = m_Markers.find(oldName);
    if (it == m_Markers.end()) {
        return Fail("no marker named '%s'", oldName.c_str());
    }

    if (m_Markers.find(newName) != m_Markers.end()) {
        return Fail("Marker '%s' already exists.", newName.c_str());
    }

    PushUndoAction(EditActionType::AddMarker, "Rename marker: " + oldName);
    RecordMarker marker = it->second;
    marker.name = newName;
    m_Markers.erase(it);
    m_Markers[newName] = marker;

    return true;
}

bool RecordPlayer::MoveMarker(const std::string &name, size_t newFrame) {
    auto it = m_Markers.find(name);
    if (it == m_Markers.end() || newFrame >= m_TotalFrames) {
        return Fail("no marker named '%s' or frame %zu is out of range", name.c_str(), newFrame);
    }

    PushUndoAction(EditActionType::AddMarker, "Move marker: " + name);
    it->second.frame = newFrame;
    return true;
}

bool RecordPlayer::SeekToMarker(const std::string &name) {
    auto it = m_Markers.find(name);
    if (it == m_Markers.end()) {
        return Fail("no marker named '%s'", name.c_str());
    }

    return Seek(it->second.frame);
}

const RecordMarker *RecordPlayer::GetNextMarker(size_t currentFrame, std::optional<MarkerType> type) const {
    const RecordMarker *closest = nullptr;
    size_t closestFrame = static_cast<size_t>(-1);

    for (const auto &pair : m_Markers) {
        const auto &marker = pair.second;
        if (marker.frame > currentFrame && marker.frame < closestFrame) {
            if (!type || marker.type == *type) {
                closest = &marker;
                closestFrame = marker.frame;
            }
        }
    }

    return closest;
}

const RecordMarker *RecordPlayer::GetPreviousMarker(size_t currentFrame, std::optional<MarkerType> type) const {
    const RecordMarker *closest = nullptr;
    size_t closestFrame = 0;

    for (const auto &pair : m_Markers) {
        const auto &marker = pair.second;
        if (marker.frame < currentFrame && marker.frame >= closestFrame) {
            if (!type || marker.type == *type) {
                closest = &marker;
                closestFrame = marker.frame;
            }
        }
    }

    return closest;
}

// --- Comment Management ---

size_t RecordPlayer::AddComment(size_t frame, const std::string &text,
                                const std::string &author, const std::string &category) {
    return AddRangeComment(frame, frame, text, author, category);
}

size_t RecordPlayer::AddRangeComment(size_t startFrame, size_t endFrame, const std::string &text,
                                     const std::string &author, const std::string &category) {
    RecordComment comment(text, startFrame);
    comment.endFrame = endFrame;
    comment.author = author;
    comment.category = category;
    comment.id = m_NextCommentId++;

    PushUndoAction(EditActionType::AddComment, "Add comment at frame " + std::to_string(startFrame));
    m_Comments.push_back(comment);

    return comment.id;
}

static auto FindComment(std::vector<RecordComment> &comments, size_t id) {
    return std::find_if(comments.begin(), comments.end(), [id](const RecordComment &c) { return c.id == id; });
}

bool RecordPlayer::RemoveComment(size_t id) {
    auto it = FindComment(m_Comments, id);
    if (it == m_Comments.end()) {
        return Fail("no comment with id %zu", id);
    }

    PushUndoAction(EditActionType::RemoveComment, "Remove comment");
    m_Comments.erase(FindComment(m_Comments, id));
    return true;
}

const RecordComment *RecordPlayer::GetComment(size_t id) const {
    for (const auto &comment : m_Comments) {
        if (comment.id == id) {
            return &comment;
        }
    }
    return nullptr;
}

std::vector<const RecordComment *> RecordPlayer::GetCommentsAtFrame(size_t frame) const {
    std::vector<const RecordComment *> result;
    for (const auto &comment : m_Comments) {
        if (frame >= comment.startFrame && frame <= comment.endFrame) {
            result.push_back(&comment);
        }
    }
    return result;
}

std::vector<RecordComment> RecordPlayer::GetAllComments() const {
    return m_Comments;
}

bool RecordPlayer::EditComment(size_t id, const std::string &newText) {
    if (FindComment(m_Comments, id) == m_Comments.end()) {
        return Fail("no comment with id %zu", id);
    }

    PushUndoAction(EditActionType::AddComment, "Edit comment");
    auto it = FindComment(m_Comments, id);
    it->text = newText;
    it->timestamp = std::time(nullptr);

    return true;
}

// --- Macro Management ---

bool RecordPlayer::SaveMacro(const std::string &name, size_t startFrame, size_t endFrame,
                             const std::string &description) {
    if (startFrame > endFrame || endFrame >= m_TotalFrames) {
        return Fail("invalid macro range %zu-%zu", startFrame, endFrame);
    }

    if (m_Macros.find(name) != m_Macros.end()) {
        return Fail("Macro '%s' already exists.", name.c_str());
    }

    RecordMacro macro(name, description);
    macro.frames.assign(m_Frames.begin() + startFrame, m_Frames.begin() + endFrame + 1);

    PushUndoAction(EditActionType::ModifyFrame, "Save macro: " + name);
    m_Macros[name] = macro;

    Log::Info("Saved macro '%s' (%zu frames)", name.c_str(), macro.frames.size());
    return true;
}

bool RecordPlayer::DeleteMacro(const std::string &name) {
    auto it = m_Macros.find(name);
    if (it == m_Macros.end()) {
        return Fail("no macro named '%s'", name.c_str());
    }

    PushUndoAction(EditActionType::ModifyFrame, "Delete macro: " + name);
    m_Macros.erase(it);
    Log::Info("Deleted macro '%s'", name.c_str());
    return true;
}

const RecordMacro *RecordPlayer::GetMacro(const std::string &name) const {
    auto it = m_Macros.find(name);
    return (it != m_Macros.end()) ? &it->second : nullptr;
}

std::vector<RecordMacro> RecordPlayer::GetAllMacros() const {
    std::vector<RecordMacro> result;
    result.reserve(m_Macros.size());
    for (const auto &pair : m_Macros) {
        result.push_back(pair.second);
    }
    return result;
}

bool RecordPlayer::InsertMacro(const std::string &name, size_t atFrame, size_t repeatCount) {
    auto it = m_Macros.find(name);
    if (it == m_Macros.end()) {
        return Fail("no macro named '%s'", name.c_str());
    }

    if (atFrame > m_TotalFrames) {
        return Fail("InsertMacro: frame %zu is out of bounds (total: %zu).", atFrame, m_TotalFrames);
    }

    if (repeatCount == 0 || it->second.frames.empty()) {
        return true;
    }

    const auto &macro = it->second;
    size_t totalFrames = macro.frames.size() * repeatCount;

    PushUndoAction(EditActionType::InsertFrames, "Insert macro: " + name);
    // Insert frames
    m_Frames.insert(m_Frames.begin() + atFrame, totalFrames, RecordFrameData());

    // Copy macro data
    for (size_t i = 0; i < repeatCount; ++i) {
        std::copy(macro.frames.begin(), macro.frames.end(),
                  m_Frames.begin() + atFrame + i * macro.frames.size());
    }

    m_TotalFrames += totalFrames;
    EnsureFrameSentinel(m_Frames, m_TotalFrames);
    ClampPlaybackCursor(m_CurrentFrame, m_TotalFrames);
    UpdateMetadataStats();
    m_IsModified = true;

    Log::Info("Inserted macro '%s' x%zu at frame %zu", name.c_str(), repeatCount, atFrame);
    return true;
}

// --- Metadata Management ---

void RecordPlayer::SetMetadataField(const std::string &field, const std::string &value) {
    PushUndoAction(EditActionType::ModifyFrame, "Set metadata: " + field);
    if (field == "title") m_Metadata.title = value;
    else if (field == "author") m_Metadata.author = value;
    else if (field == "description") m_Metadata.description = value;
    else if (field == "game_version") m_Metadata.gameVersion = value;
    else if (field == "tas_version") m_Metadata.tasVersion = value;
    else m_Metadata.customFields[field] = value;

    m_Metadata.modifiedAt = std::time(nullptr);
}

std::string RecordPlayer::GetMetadataField(const std::string &field) const {
    if (field == "title") return m_Metadata.title;
    if (field == "author") return m_Metadata.author;
    if (field == "description") return m_Metadata.description;
    if (field == "game_version") return m_Metadata.gameVersion;
    if (field == "tas_version") return m_Metadata.tasVersion;

    auto it = m_Metadata.customFields.find(field);
    return (it != m_Metadata.customFields.end()) ? it->second : "";
}

void RecordPlayer::AddTag(const std::string &tag) {
    auto it = std::find(m_Metadata.tags.begin(), m_Metadata.tags.end(), tag);
    if (it == m_Metadata.tags.end()) {
        PushUndoAction(EditActionType::ModifyFrame, "Add tag: " + tag);
        m_Metadata.tags.push_back(tag);
    }
}

void RecordPlayer::RemoveTag(const std::string &tag) {
    auto it = std::find(m_Metadata.tags.begin(), m_Metadata.tags.end(), tag);
    if (it != m_Metadata.tags.end()) {
        PushUndoAction(EditActionType::ModifyFrame, "Remove tag: " + tag);
        m_Metadata.tags.erase(it);
    }
}

// --- Undo/Redo System ---

bool RecordPlayer::Undo() {
    if (m_UndoStack.empty()) {
        return Fail("nothing to undo");
    }

    EditAction action = std::move(m_UndoStack.back());
    m_UndoStack.pop_back();
    RecordEditState after = CaptureEditState();
    RestoreEditState(std::move(action.state));
    action.state = std::move(after);
    m_RedoStack.push_back(std::move(action));

    Log::Info("Undo: %s", m_RedoStack.back().description.c_str());
    return true;
}

bool RecordPlayer::Redo() {
    if (m_RedoStack.empty()) {
        return Fail("nothing to redo");
    }

    EditAction action = std::move(m_RedoStack.back());
    m_RedoStack.pop_back();
    RecordEditState before = CaptureEditState();
    RestoreEditState(std::move(action.state));
    action.state = std::move(before);
    m_UndoStack.push_back(std::move(action));

    Log::Info("Redo: %s", m_UndoStack.back().description.c_str());
    return true;
}

std::string RecordPlayer::GetUndoDescription() const {
    return m_UndoStack.empty() ? "" : m_UndoStack.back().description;
}

std::string RecordPlayer::GetRedoDescription() const {
    return m_RedoStack.empty() ? "" : m_RedoStack.back().description;
}

void RecordPlayer::ClearHistory() {
    m_UndoStack.clear();
    m_RedoStack.clear();
}

// --- Search & Filter ---

std::vector<size_t> RecordPlayer::FindFramesWithKeys(const std::string &keyString,
                                                     size_t startFrame, size_t endFrame) const {
    std::vector<size_t> result;

    if (endFrame == static_cast<size_t>(-1)) {
        endFrame = m_TotalFrames - 1;
    }

    if (startFrame >= m_TotalFrames || endFrame >= m_TotalFrames) {
        return result;
    }

    // Same separators as the input API ("up space", "up,space", "up;space"), plus "up+space".
    std::vector<std::string> keys;
    std::string current;
    for (char c : keyString) {
        if (c == '+' || c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                keys.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (!current.empty()) {
        keys.push_back(current);
    }
    if (keys.empty()) {
        return result;
    }

    // Search frames
    for (size_t i = startFrame; i <= endFrame; ++i) {
        const auto &frame = m_Frames[i];
        bool allMatch = true;

        for (const auto &key : keys) {
            if (!GetKeyStateBit(frame.keyState, key)) {
                allMatch = false;
                break;
            }
        }

        if (allMatch) {
            result.push_back(i);
        }
    }

    return result;
}

std::vector<size_t> RecordPlayer::FindFramesByDeltaTime(float deltaTime, float tolerance) const {
    std::vector<size_t> result;

    for (size_t i = 0; i < m_TotalFrames; ++i) {
        if (std::abs(m_Frames[i].deltaTime - deltaTime) <= tolerance) {
            result.push_back(i);
        }
    }

    return result;
}

// --- Statistics & Analysis ---

std::unordered_map<std::string, float> RecordPlayer::GetStatistics(size_t startFrame,
                                                                   size_t endFrame) const {
    std::unordered_map<std::string, float> stats;

    if (endFrame == static_cast<size_t>(-1) || endFrame >= m_TotalFrames) {
        endFrame = m_TotalFrames - 1;
    }

    if (startFrame >= m_TotalFrames || startFrame > endFrame) {
        return stats;
    }

    // Calculate statistics
    float totalTime = 0.0f;
    float minDelta = (std::numeric_limits<float>::max)();
    float maxDelta = 0.0f;
    std::unordered_map<std::string, size_t> inputCounts;

    for (size_t i = startFrame; i <= endFrame; ++i) {
        const auto &frame = m_Frames[i];
        totalTime += frame.deltaTime;
        minDelta = (std::min)(minDelta, frame.deltaTime);
        maxDelta = (std::max)(maxDelta, frame.deltaTime);

        // Count inputs
        if (frame.keyState.key_up) inputCounts["up"]++;
        if (frame.keyState.key_down) inputCounts["down"]++;
        if (frame.keyState.key_left) inputCounts["left"]++;
        if (frame.keyState.key_right) inputCounts["right"]++;
        if (frame.keyState.key_shift) inputCounts["shift"]++;
        if (frame.keyState.key_space) inputCounts["space"]++;
        if (frame.keyState.key_q) inputCounts["q"]++;
        if (frame.keyState.key_esc) inputCounts["esc"]++;
        if (frame.keyState.key_enter) inputCounts["enter"]++;
    }

    size_t frameCount = endFrame - startFrame + 1;
    float avgDelta = totalTime / frameCount;

    stats["total_frames"] = static_cast<float>(frameCount);
    stats["total_time_seconds"] = totalTime / 1000.0f;
    stats["average_delta_time"] = avgDelta;
    stats["min_delta_time"] = minDelta;
    stats["max_delta_time"] = maxDelta;

    // Store input counts (using floats for consistency)
    for (const auto &pair : inputCounts) {
        stats["input_" + pair.first] = static_cast<float>(pair.second);
    }

    return stats;
}

// --- Branch Management ---

bool RecordPlayer::CreateBranch(const std::string &name, size_t divergenceFrame,
                                const std::string &description) {
    if (name.empty() || divergenceFrame >= m_TotalFrames) {
        return Fail("a branch needs a name and a frame within the record");
    }

    if (m_Branches.find(name) != m_Branches.end()) {
        return Fail("Branch '%s' already exists.", name.c_str());
    }

    RecordBranch branch;
    branch.name = name;
    branch.description = description;
    branch.divergenceFrame = divergenceFrame;
    branch.parentBranch = m_CurrentBranch;
    // A branch starts as a copy of its parent; edits made after switching to it diverge from there.
    branch.frames = CopyActiveFrames();

    m_Branches[name] = std::move(branch);

    Log::Info("Created branch '%s' at frame %zu", name.c_str(), divergenceFrame);
    return true;
}

bool RecordPlayer::DeleteBranch(const std::string &name) {
    auto it = m_Branches.find(name);
    if (it == m_Branches.end() || name == m_CurrentBranch) {
        return Fail("no branch named '%s', or it is the active branch", name.c_str());
    }

    m_Branches.erase(it);
    Log::Info("Deleted branch '%s'", name.c_str());
    return true;
}

bool RecordPlayer::SwitchBranch(const std::string &name) {
    if (name == m_CurrentBranch) {
        return true;
    }

    auto target = m_Branches.find(name);
    if (!name.empty() && target == m_Branches.end()) {
        return Fail("no branch named '%s'", name.c_str());
    }

    // Park the active frames in the branch they belong to, then bring in the target's.
    if (m_CurrentBranch.empty()) {
        m_MainBranchFrames = CopyActiveFrames();
    } else {
        m_Branches[m_CurrentBranch].frames = CopyActiveFrames();
    }
    LoadActiveFrames(name.empty() ? m_MainBranchFrames : target->second.frames);
    if (name.empty()) {
        m_MainBranchFrames.clear();
    }
    m_CurrentBranch = name;
    // Undo snapshots hold the previous branch's frames.
    ClearHistory();
    m_IsModified = true;

    Log::Info("Switched to branch '%s'", name.empty() ? "main" : name.c_str());
    return true;
}

std::vector<RecordBranch> RecordPlayer::GetAllBranches() const {
    std::vector<RecordBranch> result;
    result.reserve(m_Branches.size());
    for (const auto &pair : m_Branches) {
        result.push_back(pair.second);
    }
    return result;
}

// --- Savestate Linking ---

bool RecordPlayer::LinkSavestate(size_t frame, const std::string &savestatePath,
                                 const std::string &description) {
    if (frame >= m_TotalFrames) {
        return Fail("frame %zu is out of range (%zu frames)", frame, m_TotalFrames);
    }

    SavestateLink link(frame, savestatePath);
    link.description = description;

    PushUndoAction(EditActionType::ModifyFrame, "Link savestate at frame " + std::to_string(frame));
    m_SavestateLinks[frame] = link;

    Log::Info("Linked savestate to frame %zu", frame);
    return true;
}

bool RecordPlayer::UnlinkSavestate(size_t frame) {
    auto it = m_SavestateLinks.find(frame);
    if (it == m_SavestateLinks.end()) {
        return Fail("no savestate is linked to frame %zu", frame);
    }

    PushUndoAction(EditActionType::ModifyFrame, "Unlink savestate at frame " + std::to_string(frame));
    m_SavestateLinks.erase(it);
    Log::Info("Unlinked savestate from frame %zu", frame);
    return true;
}

const SavestateLink *RecordPlayer::GetSavestateLink(size_t frame) const {
    auto it = m_SavestateLinks.find(frame);
    return (it != m_SavestateLinks.end()) ? &it->second : nullptr;
}

std::vector<SavestateLink> RecordPlayer::GetAllSavestateLinks() const {
    std::vector<SavestateLink> result;
    result.reserve(m_SavestateLinks.size());
    for (const auto &pair : m_SavestateLinks) {
        result.push_back(pair.second);
    }
    return result;
}

// --- Helper Methods ---

void RecordPlayer::PushUndoAction(EditActionType type, const std::string &description) {
    EditAction action(type, description);
    action.state = CaptureEditState();
    m_UndoStack.push_back(std::move(action));

    // Clear redo stack when new action is performed
    m_RedoStack.clear();

    // Limit undo stack size
    while (m_UndoStack.size() > m_MaxHistorySize) {
        m_UndoStack.erase(m_UndoStack.begin());
    }
}

RecordEditState RecordPlayer::CaptureEditState() const {
    RecordEditState state;
    state.frames = CopyActiveFrames();
    state.totalFrames = m_TotalFrames;
    state.sections = m_Sections;
    state.markers = m_Markers;
    state.comments = m_Comments;
    state.macros = m_Macros;
    state.metadata = m_Metadata;
    state.savestateLinks = m_SavestateLinks;
    return state;
}

void RecordPlayer::RestoreEditState(RecordEditState &&state) {
    LoadActiveFrames(state.frames);
    m_Sections = std::move(state.sections);
    m_Markers = std::move(state.markers);
    m_Comments = std::move(state.comments);
    m_Macros = std::move(state.macros);
    m_Metadata = std::move(state.metadata);
    m_SavestateLinks = std::move(state.savestateLinks);
    m_IsModified = true;
}

std::vector<RecordFrameData> RecordPlayer::CopyActiveFrames() const {
    const size_t count = (std::min)(m_TotalFrames, m_Frames.size());
    return std::vector<RecordFrameData>(m_Frames.begin(), m_Frames.begin() + count);
}

void RecordPlayer::LoadActiveFrames(const std::vector<RecordFrameData> &frames) {
    m_Frames = frames;
    m_TotalFrames = frames.size();
    EnsureFrameSentinel(m_Frames, m_TotalFrames);
    ClampPlaybackCursor(m_CurrentFrame, m_TotalFrames);
}

void RecordPlayer::ResetEditState() {
    m_Sections.clear();
    m_Markers.clear();
    m_Comments.clear();
    m_NextCommentId = 1;
    m_Branches.clear();
    m_CurrentBranch.clear();
    m_MainBranchFrames.clear();
    m_SavestateLinks.clear();
    ClearHistory();
}

void RecordPlayer::UpdateMetadataStats() {
    m_Metadata.totalFrames = m_TotalFrames;

    float totalTime = 0.0f;
    for (const auto &frame : m_Frames) {
        totalTime += frame.deltaTime;
    }
    m_Metadata.totalTimeSeconds = totalTime / 1000.0f;

    m_Metadata.modifiedAt = std::time(nullptr);
}
