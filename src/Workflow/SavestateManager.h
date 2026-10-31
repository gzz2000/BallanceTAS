#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>

#include "VxMath.h"

#include "Result.h"
#include "ServiceContainer.h"

/**
 * @brief Savestate data structure
 *
 * Captures complete game state at a specific moment for later restoration.
 */
struct SavestateData {
    // Metadata
    std::string name;
    std::string timestamp;
    std::string levelName;
    int levelNumber;
    std::string description;

    // Physics state
    VxVector position;
    VxVector velocity;
    VxVector angularVelocity;
    VxQuaternion rotation;

    // Game state
    int points;
    int lives;
    int sector;
    float srScore;
    float hsScore;

    // Time state
    size_t tick;

    // Serialization
    std::string ToJson() const;
    static Result<SavestateData> FromJson(const std::string &json);
};

/**
 * @brief Manages game state saving and loading
 *
 * Provides savestate functionality for quick retries and experimentation.
 * Savestates are stored as JSON files in the configured TAS directory.
 *
 * Dependencies are resolved lazily via ServiceProvider (ISP narrow interfaces):
 *   IObjectProvider, IGameStateProvider, IGameQuery
 *
 * Thread Safety: Not thread-safe, must be called from game thread only.
 */
class SavestateManager {
public:
    SavestateManager(ServiceProvider &services, const std::string &directory);

    ~SavestateManager() = default;

    SavestateManager(const SavestateManager &) = delete;
    SavestateManager &operator=(const SavestateManager &) = delete;

    Result<void> SaveState(const std::string &name);
    Result<void> SaveState(const std::string &name, const std::string &description);
    Result<void> LoadState(const std::string &name);
    Result<void> DeleteState(const std::string &name);
    bool StateExists(const std::string &name) const;
    Result<std::vector<std::string>> ListStates() const;
    Result<SavestateData> GetStateInfo(const std::string &name) const;
    std::string GetSavestatesDirectory() const;

private:
    Result<SavestateData> CaptureState() const;
    Result<void> RestoreState(const SavestateData &data);
    std::string GetSavestatePath(const std::string &name) const;
    Result<void> ValidateName(const std::string &name) const;

private:
    ServiceProvider &m_Services;

    // Cache for faster lookups
    mutable std::map<std::string, SavestateData> m_StateCache;
    mutable bool m_CacheValid;

    // Savestates directory
    std::string m_SavestatesDir;
};
