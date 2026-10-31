#include "TASProject.h"

#include <filesystem>
#include <fstream>
#include <cmath>
#include <sstream>

#include <CKGlobals.h>

#include "RecordPlayer.h"

namespace fs = std::filesystem;

// Constructor for script-based projects
TASProject::TASProject(std::string projectPath, tas::lua::LuaValue manifest)
    : m_ProjectPath(std::move(projectPath)), m_Manifest(std::move(manifest)), m_ProjectType(ProjectType::Script) {
    ParseManifest(m_Manifest);
}

// Constructor for record-based projects (.tas files)
TASProject::TASProject(std::string tasFilePath)
    : m_ProjectPath(std::move(tasFilePath)), m_ProjectType(ProjectType::Record) {
    ParseRecordProject(m_ProjectPath);
}

void TASProject::ParseManifest(const tas::lua::LuaValue &manifest) {
    if (!manifest.IsTable()) {
        m_IsValid = false;
        m_ValidationMessage = "Manifest is not a table";
        return;
    }

    m_Name = manifest.GetStringField("name", "Unnamed TAS");
    m_Author = manifest.GetStringField("author", "Unknown");
    m_TargetLevel = manifest.GetStringField("level", "");
    m_EntryScript = manifest.GetStringField("entry_script", "main.lua");
    m_Description = manifest.GetStringField("description", "No description.");
    m_UpdateRate = static_cast<float>(manifest.GetNumberField("update_rate", kDefaultUpdateRate));
    if (!std::isfinite(m_UpdateRate) || m_UpdateRate <= 0.0f) {
        m_UpdateRate = kDefaultUpdateRate;
    }
    m_InitialFrameTime = static_cast<float>(manifest.GetNumberField("frame_time", 0.0));
    if (!std::isfinite(m_InitialFrameTime) || m_InitialFrameTime <= 0.0f) {
        m_InitialFrameTime = 0.0f;
    }

    // Parse project scope (default to Level for backward compatibility)
    std::string scopeStr = manifest.GetStringField("scope", "level");
    if (scopeStr == "global") {
        m_ProjectScope = ProjectScope::Global;
    } else {
        m_ProjectScope = ProjectScope::Level; // Default
    }

    // Parse execution trigger (default to level for backward compatibility)
    m_ExecutionTrigger = manifest.GetStringField("trigger", "level");
    if (m_ExecutionTrigger != "startup" && m_ExecutionTrigger != "menu" && m_ExecutionTrigger != "level") {
        m_ExecutionTrigger = "level"; // Default if invalid
    }

    if (m_Name.empty()) {
        m_IsValid = false;
        m_ValidationMessage = "Manifest requires a project name";
        return;
    }

    if (m_Author.empty()) {
        m_IsValid = false;
        m_ValidationMessage = "Manifest requires an author";
        return;
    }

    if (m_EntryScript.empty()) {
        m_IsValid = false;
        m_ValidationMessage = "Project entry script is missing";
        return;
    }

    // Validation rules:
    // - Level projects must have a target level
    // - Global projects can work without a specific target level
    if (m_ProjectScope == ProjectScope::Level) {
        // Level projects require a target level to be valid
        if (!m_TargetLevel.empty()) {
            m_IsValid = true;
        } else {
            m_IsValid = false;
            m_ValidationMessage = "Level project requires a target level";
        }
    } else {
        // Global projects are valid with just name and author (level is optional)
        m_IsValid = true;
    }

    if (m_IsValid) {
        m_ValidationMessage.clear();
    }
}

void TASProject::ParseRecordProject(const std::string &tasFilePath) {
    // Validate that the .tas file exists
    if (!fs::exists(tasFilePath) || !fs::is_regular_file(tasFilePath)) {
        m_IsValid = false;
        m_ValidationMessage = "Record file does not exist";
        return;
    }

    // Extract project name from filename (without extension)
    fs::path filePath(tasFilePath);
    m_Name = filePath.stem().string();

    // Set default values for record projects
    m_Author = "Unknown (Record)";
    m_Description = "TAS record file";
    m_TargetLevel = "";    // Will be determined during playback if possible
    m_UpdateRate = kDefaultUpdateRate; // Standard Physics rate

    // Try to parse timing and basic info from the file
    try {
        std::ifstream file(tasFilePath, std::ios::binary);
        if (!file.is_open()) {
            m_IsValid = false;
            m_ValidationMessage = "Record file cannot be opened";
            return;
        }

        // Read the 4-byte header for uncompressed size
        uint32_t uncompressedSize;
        file.read(reinterpret_cast<char *>(&uncompressedSize), sizeof(uncompressedSize));
        if (file.gcount() != sizeof(uncompressedSize)) {
            m_IsValid = false;
            m_ValidationMessage = "Record header is incomplete";
            return;
        }

        if (uncompressedSize == 0) {
            m_Description = "Empty TAS record file";
            m_IsValid = false;
            m_RecordFrameCount = 0;
            m_ValidationMessage = "Record has no frames";
            return;
        }

        // Validate that uncompressed size makes sense
        const size_t frameDataSize = sizeof(RecordFrameData);
        if (uncompressedSize % frameDataSize != 0) {
            m_IsValid = false;
            m_ValidationMessage = "Record frame data size is invalid";
            return;
        }

        // Calculate frame count
        size_t frameCount = uncompressedSize / frameDataSize;
        m_RecordFrameCount = frameCount;

        // Read the compressed payload
        file.seekg(0, std::ios::end);
        std::streampos fileSize = file.tellg();
        file.seekg(sizeof(uncompressedSize), std::ios::beg);

        size_t compressedSize = static_cast<size_t>(fileSize) - sizeof(uncompressedSize);
        if (compressedSize == 0) {
            m_IsValid = false;
            m_ValidationMessage = "Record payload is empty";
            return;
        }

        std::vector<char> compressedData(compressedSize);
        file.read(compressedData.data(), compressedSize);
        if (static_cast<size_t>(file.gcount()) != compressedSize) {
            m_IsValid = false;
            m_ValidationMessage = "Record payload is incomplete";
            return;
        }
        file.close();

        // Decompress the data
        char *uncompressedData = CKUnPackData(static_cast<int>(uncompressedSize), compressedData.data(), compressedSize);
        if (!uncompressedData) {
            m_IsValid = false;
            m_ValidationMessage = "Record payload could not be decompressed";
            return;
        }

        // Parse timing information from the frames
        if (frameCount > 0) {
            // Cast to frame data array
            const auto *frames = reinterpret_cast<const RecordFrameData *>(uncompressedData);

            bool hasConstantDeltaTime = true;
            float initialDeltaTime = frames[0].deltaTime;
            for (size_t i = 1; i < frameCount; ++i) {
                float deltaTime = frames[i].deltaTime;
                if (deltaTime != initialDeltaTime) {
                    // If delta time varies from the first frame, we need to check consistency
                    hasConstantDeltaTime = false;
                }
            }

            if (!std::isfinite(initialDeltaTime) || initialDeltaTime <= 0.0f) {
                CKDeletePointer(uncompressedData);
                m_IsValid = false;
                m_HasConstantDeltaTime = false;
                m_ValidationMessage = "Record contains invalid frame timing";
                return;
            }

            m_UpdateRate = 1000.0f / initialDeltaTime; // Convert from ms to Hz
            m_InitialFrameTime = initialDeltaTime;

            // Store delta time consistency information
            m_HasConstantDeltaTime = hasConstantDeltaTime;
        } else {
            m_HasConstantDeltaTime = true; // Empty record is technically constant
        }

        // Clean up decompressed data
        CKDeletePointer(uncompressedData);

        // Update description
        std::ostringstream desc;
        desc << "TAS record (" << frameCount << " frames)";
        m_Description = desc.str();

        m_IsValid = true;
        m_ValidationMessage.clear();
    } catch (const std::exception &) {
        // If we can't read the file properly, mark as invalid
        m_IsValid = false;
        m_HasConstantDeltaTime = false;
        m_RecordFrameCount = 0;
        m_ValidationMessage = "Invalid TAS record file";
        m_Description = "Invalid TAS record file";
    }
}

std::string TASProject::GetFileName() const {
    return fs::path(m_ProjectPath).filename().string();
}

std::string TASProject::GetFileNameWithoutExtension() const {
    return fs::path(m_ProjectPath).stem().string();
}

std::string TASProject::GetRecordFilePath() const {
    if (IsRecordProject()) {
        return m_ProjectPath; // For record projects, the project path IS the .tas file
    }
    return ""; // Script projects don't have record files
}

std::string TASProject::GetEntryScriptPath(const std::string &executionBasePath) const {
    if (IsRecordProject()) {
        return ""; // Record projects don't have entry scripts
    }
    return GetProjectFilePath(m_EntryScript, executionBasePath);
}

std::string TASProject::GetProjectFilePath(const std::string &fileName, const std::string &executionBasePath) const {
    if (IsRecordProject()) {
        return ""; // Record projects don't have project files
    }

    std::string basePath;

    if (!executionBasePath.empty()) {
        // Use provided execution base path (for zip projects)
        basePath = executionBasePath;
    } else if (!m_ExecutionBasePath.empty()) {
        // Use stored execution base path
        basePath = m_ExecutionBasePath;
    } else {
        // Use original project path (for directory projects)
        basePath = m_ProjectPath;
    }

    return (std::filesystem::path(basePath) / std::filesystem::path(fileName)).string();
}


std::string TASProject::GetTranslationCompatibilityMessage() const {
    if (!IsRecordProject()) {
        return "Not a record project";
    }

    if (!IsValid()) {
        return m_ValidationMessage.empty() ? "Invalid record file" : m_ValidationMessage;
    }

    if (m_RecordFrameCount == 0) {
        return "Record has no frames";
    }

    if (!m_HasConstantDeltaTime) {
        return "Compatible - variable timing is kept with tas.set_frame_time";
    }

    return "Compatible - constant timing detected";
}

bool TASProject::CanTranslateToRecord() const {
    return IsScriptProject() &&
           IsValid() &&
           IsLevelProject() &&
           ShouldExecuteOnLevel() &&
           !GetEntryScript().empty();
}

std::string TASProject::GetScriptToRecordCompatibilityMessage() const {
    if (!IsScriptProject()) {
        return "Only script projects can be translated to record";
    }

    if (!IsValid()) {
        return m_ValidationMessage.empty() ? "Invalid script project" : m_ValidationMessage;
    }

    if (!IsLevelProject() || !ShouldExecuteOnLevel()) {
        return "Only level script projects can be translated to record";
    }

    if (GetEntryScript().empty()) {
        return "Project entry script is missing";
    }

    return "Compatible - can be captured to record";
}

std::vector<std::string> TASProject::GetRequirements() const {
    std::vector<std::string> requirements;

    // Add translation-specific requirements for record projects
    if (IsRecordProject() && IsValid()) {
        if (!m_HasConstantDeltaTime) {
            requirements.emplace_back("Variable Timing");
        }
    }

    return requirements;
}
