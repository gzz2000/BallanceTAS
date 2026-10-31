#pragma once

#include <vector>
#include <optional>
#include <string>
#include <functional>
#include <unordered_map>
#include <ctime>
#include <memory>

#include <CKDefines.h>

// Forward declarations
class TASEngine;
class TASProject;

/**
 * @struct RecordKeyState
 * @brief Matches the exact binary format from legacy TAS records.
 */
#pragma pack(push, 1)
struct RecordKeyState {
    unsigned key_up    : 1;
    unsigned key_down  : 1;
    unsigned key_left  : 1;
    unsigned key_right : 1;
    unsigned key_shift : 1;
    unsigned key_space : 1;
    unsigned key_q     : 1;
    unsigned key_esc   : 1;
    unsigned key_enter : 1;
};
#pragma pack(pop)

/**
 * @struct RecordFrameData
 * @brief Matches the exact binary format from legacy TAS records.
 */
#pragma pack(push, 1)
struct RecordFrameData {
    float deltaTime;

    union {
        RecordKeyState keyState;
        int keyStates;
    };

    RecordFrameData() : deltaTime(0.0f), keyStates(0) {
    }

    explicit RecordFrameData(float deltaTime) : deltaTime(deltaTime) {
        keyStates = 0;
    }
};
#pragma pack(pop)

// ===================================================================
// Enhanced Record Features - Data Structures
// ===================================================================

/**
 * @struct RecordSection
 * @brief Represents a logical segment/section within a recording.
 */
struct RecordSection {
    std::string name;                                      // Section name (e.g., "Intro", "Level 1", "Boss Fight")
    std::string description;                               // Optional detailed description
    size_t startFrame;                                     // Inclusive start frame
    size_t endFrame;                                       // Inclusive end frame
    std::string color;                                     // Optional color for UI (hex format like "#FF5733")
    std::unordered_map<std::string, std::string> metadata; // Custom key-value pairs

    RecordSection() : startFrame(0), endFrame(0) {
    }

    RecordSection(std::string n, size_t start, size_t end)
        : name(std::move(n)), startFrame(start), endFrame(end) {
    }
};

/**
 * @enum MarkerType
 * @brief Types of markers that can be placed on frames.
 */
enum class MarkerType : uint8_t {
    Bookmark   = 0, // General bookmark
    Checkpoint = 1, // Important checkpoint/savestate point
    Bug        = 2, // Known issue location
    Trick      = 3, // Special trick/technique location
    Sync       = 4, // Synchronization point
    Custom     = 5  // User-defined type
};

/**
 * @struct RecordMarker
 * @brief Represents a bookmark/marker at a specific frame.
 */
struct RecordMarker {
    std::string name;                                      // Marker name (e.g., "Frame Perfect Jump")
    size_t frame;                                          // Frame number
    MarkerType type;                                       // Marker type
    std::string description;                               // Optional detailed description
    std::string color;                                     // Optional color for UI
    std::unordered_map<std::string, std::string> metadata; // Custom data

    RecordMarker() : frame(0), type(MarkerType::Bookmark) {
    }

    RecordMarker(std::string n, size_t f, MarkerType t = MarkerType::Bookmark)
        : name(std::move(n)), frame(f), type(t) {
    }
};

/**
 * @struct RecordComment
 * @brief Represents a comment/annotation on a frame or frame range.
 */
struct RecordComment {
    std::string text;      // Comment text
    size_t startFrame;     // Start frame
    size_t endFrame;       // End frame (same as startFrame for single-frame comments)
    std::string author;    // Comment author
    std::time_t timestamp; // When comment was added
    std::string category;  // Optional category/tag
    size_t id = 0;         // Stable id handed to scripts (survives removals)

    RecordComment() : startFrame(0), endFrame(0), timestamp(0) {
    }

    RecordComment(std::string txt, size_t frame)
        : text(std::move(txt)),
          startFrame(frame),
          endFrame(frame),
          timestamp(std::time(nullptr)) {
    }
};

/**
 * @struct RecordMacro
 * @brief Represents a reusable input sequence.
 */
struct RecordMacro {
    std::string name;                    // Macro name
    std::string description;             // What the macro does
    std::vector<RecordFrameData> frames; // Input sequence
    std::unordered_map<std::string, std::string> metadata;

    RecordMacro() = default;

    RecordMacro(std::string n, std::string desc)
        : name(std::move(n)), description(std::move(desc)) {
    }
};

/**
 * @struct RecordMetadata
 * @brief Extended metadata for the entire recording.
 */
struct RecordMetadata {
    std::string title;
    std::string author;
    std::string description;
    std::string gameVersion;
    std::string tasVersion;
    std::time_t createdAt;
    std::time_t modifiedAt;
    std::vector<std::string> tags;
    std::unordered_map<std::string, std::string> customFields;

    // Statistics
    size_t totalFrames;
    float totalTimeSeconds;
    size_t rerecordCount;
    std::unordered_map<std::string, size_t> inputCounts; // Key press counts

    RecordMetadata()
        : createdAt(0),
          modifiedAt(0),
          totalFrames(0),
          totalTimeSeconds(0.0f),
          rerecordCount(0) {
    }
};

/**
 * @enum EditActionType
 * @brief Types of edit actions that can be undone/redone.
 */
enum class EditActionType : uint8_t {
    InsertFrames,
    DeleteFrames,
    ModifyFrame,
    SetFrameKey,
    SetDeltaTime,
    CopyFrames,
    DuplicateFrame,
    AddSection,
    RemoveSection,
    AddMarker,
    RemoveMarker,
    AddComment,
    RemoveComment
};

/**
 * @struct RecordBranch
 * @brief Represents an alternative path/branch in the recording.
 */
struct RecordBranch {
    std::string name;
    std::string description;
    size_t divergenceFrame;              // Where this branch diverges from parent
    size_t convergenceFrame;             // Where branches merge (or -1 if no merge)
    std::string parentBranch;            // Parent branch name (empty for main branch)
    std::vector<RecordFrameData> frames; // Full frame list of this branch (without the end sentinel)

    RecordBranch() : divergenceFrame(0), convergenceFrame(static_cast<size_t>(-1)) {
    }
};

/**
 * @struct SavestateLink
 * @brief Links a frame to a game savestate for fast seeking.
 */
struct SavestateLink {
    size_t frame;
    std::string savestatePath;
    std::string description;
    std::time_t createdAt;

    SavestateLink() : frame(0), createdAt(0) {
    }

    SavestateLink(size_t f, std::string path)
        : frame(f), savestatePath(std::move(path)), createdAt(std::time(nullptr)) {
    }
};

/**
 * @struct RecordEditState
 * @brief Everything an undoable edit can change, captured before the edit runs.
 */
struct RecordEditState {
    std::vector<RecordFrameData> frames;
    size_t totalFrames = 0;
    std::unordered_map<std::string, RecordSection> sections;
    std::unordered_map<std::string, RecordMarker> markers;
    std::vector<RecordComment> comments;
    std::unordered_map<std::string, RecordMacro> macros;
    RecordMetadata metadata;
    std::unordered_map<size_t, SavestateLink> savestateLinks;
};

/**
 * @struct EditAction
 * @brief Represents a single undoable edit action.
 */
struct EditAction {
    EditActionType type;
    std::string description; // Human-readable description
    RecordEditState state;   // State on the other side of this action (before it for undo, after it for redo)

    EditAction() : type(EditActionType::ModifyFrame) {
    }

    EditAction(EditActionType t, std::string desc)
        : type(t), description(std::move(desc)) {
    }
};

/**
 * @class RecordPlayer
 * @brief Handles playback of binary .tas record files.
 *
 * This class loads and plays back TAS records in binary format (.tas files).
 * It provides frame-by-frame input replay by directly manipulating the
 * keyboard state buffer.
 */
class RecordPlayer {
public:
    explicit RecordPlayer(TASEngine *engine);
    ~RecordPlayer() = default;

    // RecordPlayer is not copyable or movable
    RecordPlayer(const RecordPlayer &) = delete;
    RecordPlayer &operator=(const RecordPlayer &) = delete;

    /**
     * @brief Loads and starts playback of a TAS record project.
     * @param project The record-based TAS project to play.
     * @return True if the record was loaded and playback started successfully.
     */
    bool LoadAndPlay(const TASProject *project);

    /**
     * @brief Stops playback and cleans up.
     */
    void Stop();

    /**
     * @brief Processes one frame of record playback.
     * This applies the input for the current frame and advances to the next.
     * Should be called from the InputManager hook.
     */
    void Tick(size_t currentTick, unsigned char *keyboardState);
    /// Apply the current legacy record frame without advancing its cursor.
    /// Used while a platform-specific level loader takes more simulation
    /// samples than the Windows player that produced the record.
    void ApplyCurrentFrame(unsigned char *keyboardState) const;
    /// Restore the input latch at a post-tick checkpoint without consuming input.
    /// The cursor denotes the next sample; paused playback is also supported.
    bool RestoreConsumedFrameInput(unsigned char *keyboardState) const;

    /**
     * @brief Checks if a record is currently playing.
     * @return True if playback is active.
     */
    bool IsPlaying() const { return m_IsPlaying; }

    /**
     * @brief Checks if playback is currently paused.
     * @return True if playback is paused.
     */
    bool IsPaused() const { return m_IsPaused; }

    /**
     * @brief Pauses the current playback.
     * Does nothing if not playing or already paused.
     */
    void Pause();

    /**
     * @brief Resumes paused playback.
     * Does nothing if not paused.
     */
    void Resume();

    /**
     * @brief Seeks to a specific frame in the record.
     * @param frame The frame number to seek to (0-based).
     * @return True if seek was successful, false if frame is out of bounds or not playing.
     */
    bool Seek(size_t frame);

    /**
     * @brief Gets the current playback frame.
     * @return The current frame number (0-based), or 0 if not playing.
     */
    size_t GetCurrentFrame() const { return m_CurrentFrame; }

    /**
     * @brief Gets the total number of frames in the loaded record.
     * @return Total frame count, or 0 if no record is loaded.
     */
    size_t GetTotalFrames() const { return m_TotalFrames; }

    /**
     * @brief Loads and starts playback of a record file directly by path.
     * @param recordPath Path to the .tas file.
     * @return True if the record was loaded and playback started successfully.
     */
    bool LoadAndPlay(const std::string &recordPath);

    /**
     * @brief Gets the delta time for the current frame.
     * Used by TimeManager hook to set the correct frame timing.
     * @return Delta time in milliseconds for the current frame.
     */
    float GetFrameDeltaTime(size_t currentTick) const;

    /**
     * @brief Sets a callback to be called when playback status changes.
     * @param callback Function called with true when starting, false when stopping.
     */
    void SetStatusCallback(std::function<void(bool)> callback) {
        m_StatusCallback = std::move(callback);
    }

    // ===================================================================
    // Frame-Level Input Query & Modification
    // ===================================================================

    /**
     * @brief Gets the input data for a specific frame.
     * @param frame The frame number (0-based).
     * @param outData Pointer to receive the frame data.
     * @return True if successful, false if frame is out of bounds or no record is loaded.
     */
    bool GetFrameInput(size_t frame, RecordFrameData *outData) const;

    /**
     * @brief Sets the input data for a specific frame.
     * @param frame The frame number (0-based).
     * @param inputData The new input data for the frame.
     * @return True if successful, false if frame is out of bounds or no record is loaded.
     */
    bool SetFrameInput(size_t frame, const RecordFrameData &inputData);

    /**
     * @brief Sets a specific key state for a frame.
     * @param frame The frame number (0-based).
     * @param key The key name (e.g., "up", "down", "space").
     * @param pressed True to press the key, false to release it.
     * @return True if successful, false if frame/key is invalid.
     */
    bool SetFrameKey(size_t frame, const std::string &key, bool pressed);

    /**
     * @brief Gets the delta time for a specific frame by frame number.
     * @param frame The frame number (0-based).
     * @return The delta time in milliseconds, or 0.0f if frame is invalid.
     */
    float GetFrameDeltaTimeByFrame(size_t frame) const;

    /**
     * @brief Sets the delta time for a specific frame.
     * @param frame The frame number (0-based).
     * @param deltaTime The new delta time in milliseconds.
     * @return True if successful, false if frame is out of bounds.
     */
    bool SetFrameDeltaTime(size_t frame, float deltaTime);

    // ===================================================================
    // Bulk Frame Operations
    // ===================================================================

    /**
     * @brief Inserts blank frames at a specific position.
     * @param startFrame The position to insert frames (0-based).
     * @param count Number of frames to insert.
     * @return True if successful, false on error.
     */
    bool InsertFrames(size_t startFrame, size_t count);

    /**
     * @brief Deletes a range of frames.
     * @param startFrame The first frame to delete (0-based).
     * @param count Number of frames to delete.
     * @return True if successful, false on error.
     */
    bool DeleteFrames(size_t startFrame, size_t count);

    /**
     * @brief Copies a range of frames to another position.
     * @param srcStart Source starting frame (0-based).
     * @param destStart Destination starting frame (0-based).
     * @param count Number of frames to copy.
     * @return True if successful, false on error.
     */
    bool CopyFrames(size_t srcStart, size_t destStart, size_t count);

    /**
     * @brief Duplicates a single frame multiple times.
     * @param frame The frame to duplicate (0-based).
     * @param count Number of times to duplicate.
     * @return True if successful, false on error.
     */
    bool DuplicateFrame(size_t frame, size_t count);

    // ===================================================================
    // Advanced Playback Control
    // ===================================================================

    /**
     * @brief Sets the playback speed multiplier.
     * @param speed Speed multiplier (1.0 = normal, 2.0 = 2x speed, 0.5 = half speed).
     */
    void SetPlaybackSpeed(float speed) { m_PlaybackSpeed = speed; }

    /**
     * @brief Gets the current playback speed multiplier.
     * @return The playback speed multiplier.
     */
    float GetPlaybackSpeed() const { return m_PlaybackSpeed; }

    /**
     * @brief Seeks relative to the current position.
     * @param offset Number of frames to seek (positive = forward, negative = backward).
     * @return True if successful, false if seeking would go out of bounds.
     */
    bool SeekRelative(int offset);

    /**
     * @brief Checks if playback is at the last frame.
     * @return True if at the last frame, false otherwise.
     */
    bool IsAtEnd() const { return m_CurrentFrame >= m_TotalFrames; }

    /**
     * @brief Gets the playback progress as a percentage.
     * @return Progress from 0.0 to 1.0, or 0.0 if no record is loaded.
     */
    float GetProgress() const;

    // ===================================================================
    // Input Display & Analysis
    // ===================================================================

    /**
     * @brief Gets a human-readable string representation of frame inputs.
     * @param frame The frame number (0-based).
     * @return String representation (e.g., "Up+Right Space"), or empty if invalid.
     */
    std::string GetInputString(size_t frame) const;

    /**
     * @brief Gets a list of pressed keys for a specific frame.
     * @param frame The frame number (0-based).
     * @return Vector of key names that are pressed in the frame.
     */
    std::vector<std::string> GetPressedKeys(size_t frame) const;

    /**
     * @brief Finds the next frame where a key's state changes.
     * @param startFrame Frame to start searching from (0-based).
     * @param key Key name to search for.
     * @return Frame number where the key state changes, or -1 if not found.
     */
    int FindInputChange(size_t startFrame, const std::string &key) const;

    // ===================================================================
    // Validation & Comparison
    // ===================================================================

    /**
     * @brief Compares two frames for equality.
     * @param frame1 First frame number (0-based).
     * @param frame2 Second frame number (0-based).
     * @return True if frames have identical input data, false otherwise.
     */
    bool CompareFrames(size_t frame1, size_t frame2) const;

    /**
     * @brief Validates the integrity of the loaded record.
     * @return True if the record is valid, false if there are issues.
     */
    bool Validate() const;

    // ===================================================================
    // Save & Export Operations
    // ===================================================================

    /**
     * @brief Saves the current record to a file.
     * @param path Path to save the .tas file.
     * @return True if successful, false on error.
     */
    bool Save(const std::string &path);

    /// Why the last operation that returned false failed.
    const std::string &GetLastError() const { return m_LastError; }
    void ClearLastError() { m_LastError.clear(); }

    /// Sidecar file next to a record that keeps what the .tas format cannot hold.
    static std::string GetAnnotationsPath(const std::string &recordPath);
    bool SaveAnnotations(const std::string &path) const;
    bool LoadAnnotations(const std::string &path);
    bool HasAnnotations() const;

    /**
     * @brief Exports frame inputs to a human-readable format.
     * @param path Path to save the export file.
     * @param format Export format ("txt" or "json").
     * @return True if successful, false on error.
     */
    bool ExportInputs(const std::string &path, const std::string &format);

    // ===================================================================
    // Section Management
    // ===================================================================

    /**
     * @brief Adds a section to the recording.
     * @param startFrame Start frame (inclusive).
     * @param endFrame End frame (inclusive).
     * @param name Section name.
     * @param description Optional description.
     * @param color Optional color (hex format).
     * @return True if successful, false if invalid range or duplicate name.
     */
    bool AddSection(size_t startFrame, size_t endFrame, const std::string &name,
                    const std::string &description = "", const std::string &color = "");

    /**
     * @brief Removes a section by name.
     * @param name Section name.
     * @return True if successful, false if section not found.
     */
    bool RemoveSection(const std::string &name);

    /**
     * @brief Gets a section by name.
     * @param name Section name.
     * @return Pointer to section or nullptr if not found.
     */
    const RecordSection *GetSection(const std::string &name) const;

    /**
     * @brief Gets all sections.
     * @return Vector of all sections.
     */
    std::vector<RecordSection> GetAllSections() const;

    /**
     * @brief Gets the section at a specific frame.
     * @param frame Frame number.
     * @return Pointer to section or nullptr if no section at that frame.
     */
    const RecordSection *GetSectionAtFrame(size_t frame) const;

    /**
     * @brief Renames a section.
     * @param oldName Old section name.
     * @param newName New section name.
     * @return True if successful.
     */
    bool RenameSection(const std::string &oldName, const std::string &newName);

    /**
     * @brief Moves a section to a new start frame (preserves length).
     * @param name Section name.
     * @param newStartFrame New start frame.
     * @return True if successful.
     */
    bool MoveSection(const std::string &name, size_t newStartFrame);

    /**
     * @brief Resizes a section.
     * @param name Section name.
     * @param newStartFrame New start frame.
     * @param newEndFrame New end frame.
     * @return True if successful.
     */
    bool ResizeSection(const std::string &name, size_t newStartFrame, size_t newEndFrame);

    // ===================================================================
    // Marker Management
    // ===================================================================

    /**
     * @brief Adds a marker to a frame.
     * @param frame Frame number.
     * @param name Marker name.
     * @param type Marker type.
     * @param description Optional description.
     * @param color Optional color.
     * @return True if successful, false if duplicate name.
     */
    bool AddMarker(size_t frame, const std::string &name, MarkerType type = MarkerType::Bookmark,
                   const std::string &description = "", const std::string &color = "");

    /**
     * @brief Removes a marker by name.
     * @param name Marker name.
     * @return True if successful.
     */
    bool RemoveMarker(const std::string &name);

    /**
     * @brief Gets a marker by name.
     * @param name Marker name.
     * @return Pointer to marker or nullptr if not found.
     */
    const RecordMarker *GetMarker(const std::string &name) const;

    /**
     * @brief Gets all markers.
     * @return Vector of all markers.
     */
    std::vector<RecordMarker> GetAllMarkers() const;

    /**
     * @brief Gets all markers at a specific frame.
     * @param frame Frame number.
     * @return Vector of markers at the frame.
     */
    std::vector<RecordMarker> GetMarkersAtFrame(size_t frame) const;

    /**
     * @brief Gets all markers in a frame range.
     * @param startFrame Start frame.
     * @param endFrame End frame.
     * @return Vector of markers in range.
     */
    std::vector<RecordMarker> GetMarkersInRange(size_t startFrame, size_t endFrame) const;

    /**
     * @brief Renames a marker.
     * @param oldName Old marker name.
     * @param newName New marker name.
     * @return True if successful.
     */
    bool RenameMarker(const std::string &oldName, const std::string &newName);

    /**
     * @brief Moves a marker to a new frame.
     * @param name Marker name.
     * @param newFrame New frame number.
     * @return True if successful.
     */
    bool MoveMarker(const std::string &name, size_t newFrame);

    /**
     * @brief Seeks to a marker by name.
     * @param name Marker name.
     * @return True if successful.
     */
    bool SeekToMarker(const std::string &name);

    /**
     * @brief Gets the next marker after a frame (optionally filtered by type).
     * @param currentFrame Current frame.
     * @param type Optional marker type filter.
     * @return Pointer to next marker or nullptr if none found.
     */
    const RecordMarker *GetNextMarker(size_t currentFrame, std::optional<MarkerType> type = std::nullopt) const;

    /**
     * @brief Gets the previous marker before a frame (optionally filtered by type).
     * @param currentFrame Current frame.
     * @param type Optional marker type filter.
     * @return Pointer to previous marker or nullptr if none found.
     */
    const RecordMarker *GetPreviousMarker(size_t currentFrame, std::optional<MarkerType> type = std::nullopt) const;

    // ===================================================================
    // Comment Management
    // ===================================================================

    /**
     * @brief Adds a comment to a frame.
     * @param frame Frame number.
     * @param text Comment text.
     * @param author Optional author.
     * @param category Optional category.
     * @return Stable comment id.
     */
    size_t AddComment(size_t frame, const std::string &text,
                      const std::string &author = "", const std::string &category = "");

    /**
     * @brief Adds a comment to a frame range.
     * @param startFrame Start frame.
     * @param endFrame End frame.
     * @param text Comment text.
     * @param author Optional author.
     * @param category Optional category.
     * @return Stable comment id.
     */
    size_t AddRangeComment(size_t startFrame, size_t endFrame, const std::string &text,
                           const std::string &author = "", const std::string &category = "");

    /**
     * @brief Removes a comment by index.
     * @param id Comment id returned by AddComment/AddRangeComment.
     * @return True if successful.
     */
    bool RemoveComment(size_t id);

    /**
     * @brief Gets a comment by index.
     * @param id Comment id returned by AddComment/AddRangeComment.
     * @return Pointer to comment or nullptr if no comment has that id.
     */
    const RecordComment *GetComment(size_t id) const;

    /**
     * @brief Gets all comments at a specific frame.
     * @param frame Frame number.
     * @return Vector of comments at the frame.
     */
    std::vector<const RecordComment *> GetCommentsAtFrame(size_t frame) const;

    /**
     * @brief Gets all comments.
     * @return Vector of all comments.
     */
    std::vector<RecordComment> GetAllComments() const;

    /**
     * @brief Edits a comment's text.
     * @param id Comment id returned by AddComment/AddRangeComment.
     * @param newText New comment text.
     * @return True if successful.
     */
    bool EditComment(size_t id, const std::string &newText);

    // ===================================================================
    // Macro Management
    // ===================================================================

    /**
     * @brief Saves a frame range as a macro.
     * @param name Macro name.
     * @param startFrame Start frame.
     * @param endFrame End frame.
     * @param description Optional description.
     * @return True if successful.
     */
    bool SaveMacro(const std::string &name, size_t startFrame, size_t endFrame,
                   const std::string &description = "");

    /**
     * @brief Deletes a macro.
     * @param name Macro name.
     * @return True if successful.
     */
    bool DeleteMacro(const std::string &name);

    /**
     * @brief Gets a macro by name.
     * @param name Macro name.
     * @return Pointer to macro or nullptr if not found.
     */
    const RecordMacro *GetMacro(const std::string &name) const;

    /**
     * @brief Gets all macros.
     * @return Vector of all macros.
     */
    std::vector<RecordMacro> GetAllMacros() const;

    /**
     * @brief Inserts a macro at a specific frame.
     * @param name Macro name.
     * @param atFrame Frame to insert at.
     * @param repeatCount Number of times to repeat the macro.
     * @return True if successful.
     */
    bool InsertMacro(const std::string &name, size_t atFrame, size_t repeatCount = 1);

    // ===================================================================
    // Metadata Management
    // ===================================================================

    /**
     * @brief Sets a metadata field.
     * @param field Field name.
     * @param value Field value.
     */
    void SetMetadataField(const std::string &field, const std::string &value);

    /**
     * @brief Gets a metadata field.
     * @param field Field name.
     * @return Field value or empty string if not found.
     */
    std::string GetMetadataField(const std::string &field) const;

    /**
     * @brief Gets the full metadata structure.
     * @return Const reference to metadata.
     */
    const RecordMetadata &GetMetadata() const { return m_Metadata; }

    /**
     * @brief Adds a tag to the recording.
     * @param tag Tag to add.
     */
    void AddTag(const std::string &tag);

    /**
     * @brief Removes a tag from the recording.
     * @param tag Tag to remove.
     */
    void RemoveTag(const std::string &tag);

    /**
     * @brief Gets all tags.
     * @return Vector of tags.
     */
    std::vector<std::string> GetTags() const { return m_Metadata.tags; }

    /**
     * @brief Increments the rerecord count.
     */
    void IncrementRerecordCount() { m_Metadata.rerecordCount++; }

    /**
     * @brief Gets the rerecord count.
     * @return Rerecord count.
     */
    size_t GetRerecordCount() const { return m_Metadata.rerecordCount; }

    // ===================================================================
    // Undo/Redo System
    // ===================================================================

    /**
     * @brief Undoes the last edit action.
     * @return True if successful.
     */
    bool Undo();

    /**
     * @brief Redoes the last undone action.
     * @return True if successful.
     */
    bool Redo();

    /**
     * @brief Checks if undo is available.
     * @return True if can undo.
     */
    bool CanUndo() const { return !m_UndoStack.empty(); }

    /**
     * @brief Checks if redo is available.
     * @return True if can redo.
     */
    bool CanRedo() const { return !m_RedoStack.empty(); }

    /**
     * @brief Gets the description of the next undo action.
     * @return Description or empty string if no undo available.
     */
    std::string GetUndoDescription() const;

    /**
     * @brief Gets the description of the next redo action.
     * @return Description or empty string if no redo available.
     */
    std::string GetRedoDescription() const;

    /**
     * @brief Clears all undo/redo history.
     */
    void ClearHistory();

    /**
     * @brief Gets the current undo stack size.
     * @return Number of undo actions available.
     */
    size_t GetHistorySize() const { return m_UndoStack.size(); }

    /**
     * @brief Sets the maximum history size.
     * @param size Maximum number of undo actions to keep.
     */
    void SetMaxHistorySize(size_t size) { m_MaxHistorySize = size; }

    // ===================================================================
    // Search & Filter
    // ===================================================================

    /**
     * @brief Finds frames with specific keys pressed.
     * @param keyString Key string (e.g., "up+space").
     * @param startFrame Optional start frame.
     * @param endFrame Optional end frame.
     * @return Vector of frame numbers.
     */
    std::vector<size_t> FindFramesWithKeys(const std::string &keyString,
                                           size_t startFrame = 0, size_t endFrame = static_cast<size_t>(-1)) const;

    /**
     * @brief Finds frames by delta time.
     * @param deltaTime Target delta time.
     * @param tolerance Tolerance for matching.
     * @return Vector of frame numbers.
     */
    std::vector<size_t> FindFramesByDeltaTime(float deltaTime, float tolerance = 0.01f) const;

    // ===================================================================
    // Statistics & Analysis
    // ===================================================================

    /**
     * @brief Gets statistics for a frame range.
     * @param startFrame Start frame (0 = from beginning).
     * @param endFrame End frame (-1 = to end).
     * @return Statistics structure (returned as unordered_map for Lua compatibility).
     */
    std::unordered_map<std::string, float> GetStatistics(size_t startFrame = 0,
                                                         size_t endFrame = static_cast<size_t>(-1)) const;

    // ===================================================================
    // Branch Management
    // ===================================================================

    /**
     * @brief Creates a new branch.
     * @param name Branch name.
     * @param divergenceFrame Frame where branch diverges.
     * @param description Optional description.
     * @return True if successful.
     */
    bool CreateBranch(const std::string &name, size_t divergenceFrame,
                      const std::string &description = "");

    /**
     * @brief Deletes a branch.
     * @param name Branch name.
     * @return True if successful.
     */
    bool DeleteBranch(const std::string &name);

    /**
     * @brief Switches to a different branch.
     * @param name Branch name.
     * @return True if successful.
     */
    bool SwitchBranch(const std::string &name);

    /**
     * @brief Gets the current branch name.
     * @return Current branch name (empty for main branch).
     */
    std::string GetCurrentBranch() const { return m_CurrentBranch; }

    /**
     * @brief Gets all branches.
     * @return Vector of branches.
     */
    std::vector<RecordBranch> GetAllBranches() const;

    // ===================================================================
    // Savestate Linking
    // ===================================================================

    /**
     * @brief Links a savestate to a frame.
     * @param frame Frame number.
     * @param savestatePath Path to savestate file.
     * @param description Optional description.
     * @return True if successful.
     */
    bool LinkSavestate(size_t frame, const std::string &savestatePath,
                       const std::string &description = "");

    /**
     * @brief Unlinks a savestate from a frame.
     * @param frame Frame number.
     * @return True if successful.
     */
    bool UnlinkSavestate(size_t frame);

    /**
     * @brief Gets the savestate link for a frame.
     * @param frame Frame number.
     * @return Pointer to savestate link or nullptr if not found.
     */
    const SavestateLink *GetSavestateLink(size_t frame) const;

    /**
     * @brief Gets all savestate links.
     * @return Vector of all savestate links.
     */
    std::vector<SavestateLink> GetAllSavestateLinks() const;

private:
    /**
     * @brief Notifies status change via callback.
     * @param isPlaying True if starting playback, false if stopping.
     */
    void NotifyStatusChange(bool isPlaying) const {
        if (m_StatusCallback) {
            m_StatusCallback(isPlaying);
        }
    }

    /**
     * @brief Loads a .tas record file using the legacy format.
     * @param recordPath Path to the .tas file.
     * @return True if the file was loaded successfully.
     */
    bool LoadRecord(const std::string &recordPath);
    bool SaveFrames(const std::string &path);

    /**
     * @brief Applies legacy keyboard state input for the current frame.
     * @param frameData The current frame's input data.
     * @param keyboardState The game's keyboard state buffer.
     */
    void ApplyFrameInput(const RecordFrameData &currentFrame,
                         const RecordFrameData &nextFrame,
                         unsigned char *keyboardState) const;
    static unsigned char ConvertKeyState(bool current, bool next);

    /**
     * @brief Gets the key state bit for a given key name.
     * @param keyState The key state struct to query.
     * @param keyName The name of the key (e.g., "up", "down", "space").
     * @return True if the key is pressed in the given state, false otherwise.
     */
    static bool GetKeyStateBit(const RecordKeyState &keyState, const std::string &keyName);

    /**
     * @brief Sets the key state bit for a given key name.
     * @param keyState The key state struct to modify.
     * @param keyName The name of the key (e.g., "up", "down", "space").
     * @param pressed True to press the key, false to release it.
     * @return True if the key name was valid, false otherwise.
     */
    static bool SetKeyStateBit(RecordKeyState &keyState, const std::string &keyName, bool pressed);

    // Core references
    TASEngine *m_Engine;

    // Record data
    size_t m_TotalFrames = 0;
    size_t m_CurrentFrame = 0;
    std::vector<RecordFrameData> m_Frames;
    bool m_IsPlaying = false;
    bool m_IsPaused = false;
    float m_PlaybackSpeed = 1.0f; // Playback speed multiplier
    bool m_IsModified = false;    // Track if record has been modified

    // Callback for execution status changes
    std::function<void(bool)> m_StatusCallback;

    // Cached remapped keys (acquired once when playback starts)
    CKKEYBOARD m_KeyUp = CKKEY_UP;
    CKKEYBOARD m_KeyDown = CKKEY_DOWN;
    CKKEYBOARD m_KeyLeft = CKKEY_LEFT;
    CKKEYBOARD m_KeyRight = CKKEY_RIGHT;
    CKKEYBOARD m_KeyShift = CKKEY_LSHIFT;
    CKKEYBOARD m_KeySpace = CKKEY_SPACE;

    // ===================================================================
    // Enhanced Feature Data
    // ===================================================================

    // Section management
    std::unordered_map<std::string, RecordSection> m_Sections;

    // Marker management
    std::unordered_map<std::string, RecordMarker> m_Markers;

    // Comment management
    std::vector<RecordComment> m_Comments;
    size_t m_NextCommentId = 1;

    // Macro management
    std::unordered_map<std::string, RecordMacro> m_Macros;

    // Metadata
    RecordMetadata m_Metadata;

    // Undo/Redo system
    std::vector<EditAction> m_UndoStack;
    std::vector<EditAction> m_RedoStack;
    size_t m_MaxHistorySize = 100;

    // Branch management
    std::unordered_map<std::string, RecordBranch> m_Branches;
    std::string m_CurrentBranch; // Empty string = main branch
    std::vector<RecordFrameData> m_MainBranchFrames; // Main branch frames while another branch is active

    // Savestate links
    std::unordered_map<size_t, SavestateLink> m_SavestateLinks;

    mutable std::string m_LastError;

    // Helper methods
    /// Logs the formatted message as a warning (callers get it back as the failure reason),
    /// keeps it for GetLastError and returns false.
    bool Fail(const char *format, ...) const;
    /// Snapshots the edit state; call after validating an edit and before mutating anything.
    void PushUndoAction(EditActionType type, const std::string &description);
    RecordEditState CaptureEditState() const;
    void RestoreEditState(RecordEditState &&state);
    std::vector<RecordFrameData> CopyActiveFrames() const;
    void LoadActiveFrames(const std::vector<RecordFrameData> &frames);
    /// Drops annotations, history and branches that belonged to the previous record.
    void ResetEditState();
    void UpdateMetadataStats();
};
