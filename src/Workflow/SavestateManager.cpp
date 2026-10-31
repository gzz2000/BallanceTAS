#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <regex>

#include <CKAll.h>
#include <yyjson.h>

#include "Logger.h"
#include "IObjectProvider.h"
#include "IGameStateProvider.h"
#include "IGameQuery.h"
#include "SavestateManager.h"
#include "ServiceContainer.h"
#include "physics_RT.h"

namespace fs = std::filesystem;

// ============================================================================
// SavestateData Serialization
// ============================================================================

std::string SavestateData::ToJson() const {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    // Metadata
    yyjson_mut_obj_add_str(doc, root, "name", name.c_str());
    yyjson_mut_obj_add_str(doc, root, "timestamp", timestamp.c_str());
    yyjson_mut_obj_add_str(doc, root, "levelName", levelName.c_str());
    yyjson_mut_obj_add_int(doc, root, "levelNumber", levelNumber);
    yyjson_mut_obj_add_str(doc, root, "description", description.c_str());

    // Physics state
    yyjson_mut_val *pos_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_real(doc, pos_obj, "x", position.x);
    yyjson_mut_obj_add_real(doc, pos_obj, "y", position.y);
    yyjson_mut_obj_add_real(doc, pos_obj, "z", position.z);
    yyjson_mut_obj_add_val(doc, root, "position", pos_obj);

    yyjson_mut_val *vel_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_real(doc, vel_obj, "x", velocity.x);
    yyjson_mut_obj_add_real(doc, vel_obj, "y", velocity.y);
    yyjson_mut_obj_add_real(doc, vel_obj, "z", velocity.z);
    yyjson_mut_obj_add_val(doc, root, "velocity", vel_obj);

    yyjson_mut_val *angVel_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_real(doc, angVel_obj, "x", angularVelocity.x);
    yyjson_mut_obj_add_real(doc, angVel_obj, "y", angularVelocity.y);
    yyjson_mut_obj_add_real(doc, angVel_obj, "z", angularVelocity.z);
    yyjson_mut_obj_add_val(doc, root, "angularVelocity", angVel_obj);

    yyjson_mut_val *rot_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_real(doc, rot_obj, "x", rotation.x);
    yyjson_mut_obj_add_real(doc, rot_obj, "y", rotation.y);
    yyjson_mut_obj_add_real(doc, rot_obj, "z", rotation.z);
    yyjson_mut_obj_add_real(doc, rot_obj, "w", rotation.w);
    yyjson_mut_obj_add_val(doc, root, "rotation", rot_obj);

    // Game state
    yyjson_mut_obj_add_int(doc, root, "points", points);
    yyjson_mut_obj_add_int(doc, root, "lives", lives);
    yyjson_mut_obj_add_int(doc, root, "sector", sector);
    yyjson_mut_obj_add_real(doc, root, "srScore", srScore);
    yyjson_mut_obj_add_real(doc, root, "hsScore", hsScore);

    // Time state
    yyjson_mut_obj_add_uint(doc, root, "tick", static_cast<uint64_t>(tick));

    // Write to string
    const char *json_str = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    std::string result(json_str);
    free((void *) json_str);
    yyjson_mut_doc_free(doc);

    return result;
}

Result<SavestateData> SavestateData::FromJson(const std::string &json) {
    yyjson_doc *doc = yyjson_read(json.c_str(), json.length(), 0);
    if (!doc) {
        return Result<SavestateData>::Error("Failed to parse JSON");
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return Result<SavestateData>::Error("JSON root is not an object");
    }

    SavestateData data;

    // Helper to get string
    auto get_str = [&](yyjson_val *object, const char *key) -> std::string {
        yyjson_val *val = object ? yyjson_obj_get(object, key) : nullptr;
        if (val && yyjson_is_str(val)) {
            return std::string(yyjson_get_str(val));
        }
        return "";
    };

    // Helper to get int
    auto get_int = [&](yyjson_val *object, const char *key, int default_val = 0) -> int {
        yyjson_val *val = object ? yyjson_obj_get(object, key) : nullptr;
        if (val && yyjson_is_num(val)) {
            return yyjson_get_int(val);
        }
        return default_val;
    };

    // Helper to get float
    auto get_real = [&](yyjson_val *object, const char *key, float default_val = 0.0f) -> float {
        yyjson_val *val = object ? yyjson_obj_get(object, key) : nullptr;
        if (val && yyjson_is_num(val)) {
            return static_cast<float>(yyjson_get_real(val));
        }
        return default_val;
    };

    // Metadata
    data.name = get_str(root, "name");
    data.timestamp = get_str(root, "timestamp");
    data.levelName = get_str(root, "levelName");
    data.levelNumber = get_int(root, "levelNumber");
    data.description = get_str(root, "description");

    // Physics state
    yyjson_val *pos_obj = yyjson_obj_get(root, "position");
    if (pos_obj && yyjson_is_obj(pos_obj)) {
        data.position.x = get_real(pos_obj, "x");
        data.position.y = get_real(pos_obj, "y");
        data.position.z = get_real(pos_obj, "z");
    }

    yyjson_val *vel_obj = yyjson_obj_get(root, "velocity");
    if (vel_obj && yyjson_is_obj(vel_obj)) {
        data.velocity.x = get_real(vel_obj, "x");
        data.velocity.y = get_real(vel_obj, "y");
        data.velocity.z = get_real(vel_obj, "z");
    }

    yyjson_val *angVel_obj = yyjson_obj_get(root, "angularVelocity");
    if (angVel_obj && yyjson_is_obj(angVel_obj)) {
        data.angularVelocity.x = get_real(angVel_obj, "x");
        data.angularVelocity.y = get_real(angVel_obj, "y");
        data.angularVelocity.z = get_real(angVel_obj, "z");
    }

    yyjson_val *rot_obj = yyjson_obj_get(root, "rotation");
    if (rot_obj && yyjson_is_obj(rot_obj)) {
        data.rotation.x = get_real(rot_obj, "x");
        data.rotation.y = get_real(rot_obj, "y");
        data.rotation.z = get_real(rot_obj, "z");
        data.rotation.w = get_real(rot_obj, "w", 1.0f);
    }

    // Game state
    data.points = get_int(root, "points");
    data.lives = get_int(root, "lives");
    data.sector = get_int(root, "sector");
    data.srScore = get_real(root, "srScore");
    data.hsScore = get_real(root, "hsScore");

    // Time state
    yyjson_val *tick_val = yyjson_obj_get(root, "tick");
    if (tick_val && yyjson_is_uint(tick_val)) {
        data.tick = static_cast<size_t>(yyjson_get_uint(tick_val));
    }

    yyjson_doc_free(doc);
    return Result<SavestateData>::Ok(data);
}

// ============================================================================
// SavestateManager Implementation
// ============================================================================

SavestateManager::SavestateManager(ServiceProvider &services, const std::string &directory)
    : m_Services(services),
      m_CacheValid(false),
      m_SavestatesDir(directory) {
    // Share the engine's configured user-data root with recordings/scripts.
    // A separate relative default bypasses --user-data-path on native hosts.

    // Create directory if it doesn't exist
    try {
        fs::create_directories(m_SavestatesDir);
        Log::Info("Savestate directory: %s", m_SavestatesDir.c_str());
    } catch (const fs::filesystem_error &e) {
        Log::Error("Failed to create savestates directory: %s", e.what());
    }
}

Result<void> SavestateManager::SaveState(const std::string &name) {
    return SaveState(name, "");
}

Result<void> SavestateManager::SaveState(const std::string &name, const std::string &description) {
    auto validate_result = ValidateName(name);
    if (validate_result.IsError()) {
        return validate_result;
    }

    auto capture_result = CaptureState();
    if (capture_result.IsError()) {
        return Result<void>::Error(capture_result.GetError());
    }

    SavestateData data = capture_result.Unwrap();
    data.name = name;
    data.description = description;

    std::string json = data.ToJson();

    std::string filepath = GetSavestatePath(name);
    try {
        std::ofstream file(filepath);
        if (!file.is_open()) {
            return Result<void>::Error("Failed to open file for writing: " + filepath);
        }

        file << json;
        file.close();

        Log::Info("Savestate saved: %s", name.c_str());

        m_StateCache[name] = data;
        m_CacheValid = true;

        return Result<void>::Ok();
    } catch (const std::exception &e) {
        return Result<void>::Error("Failed to write savestate file: " + std::string(e.what()));
    }
}

Result<void> SavestateManager::LoadState(const std::string &name) {
    auto validateResult = ValidateName(name);
    if (validateResult.IsError()) {
        return validateResult;
    }

    if (!StateExists(name)) {
        return Result<void>::Error("Savestate does not exist: " + name);
    }

    std::string filepath = GetSavestatePath(name);
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return Result<void>::Error("Failed to open file for reading: " + filepath);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string json = buffer.str();
        file.close();

        auto dataResult = SavestateData::FromJson(json);
        if (dataResult.IsError()) {
            return Result<void>::Error("Failed to parse savestate: " + dataResult.GetError().message);
        }

        const SavestateData &data = dataResult.Unwrap();

        auto restoreResult = RestoreState(data);
        if (restoreResult.IsError()) {
            return restoreResult;
        }

        Log::Info("Savestate loaded: %s", name.c_str());
        return Result<void>::Ok();
    } catch (const std::exception &e) {
        return Result<void>::Error("Failed to read savestate file: " + std::string(e.what()));
    }
}

Result<void> SavestateManager::DeleteState(const std::string &name) {
    auto validateResult = ValidateName(name);
    if (validateResult.IsError()) {
        return validateResult;
    }

    std::string filepath = GetSavestatePath(name);

    try {
        if (fs::exists(filepath)) {
            fs::remove(filepath);
            Log::Info("Savestate deleted: %s", name.c_str());

            m_StateCache.erase(name);

            return Result<void>::Ok();
        } else {
            return Result<void>::Error("Savestate does not exist: " + name);
        }
    } catch (const fs::filesystem_error &e) {
        return Result<void>::Error("Failed to delete savestate: " + std::string(e.what()));
    }
}

bool SavestateManager::StateExists(const std::string &name) const {
    std::string filepath = GetSavestatePath(name);
    return fs::exists(filepath);
}

Result<std::vector<std::string>> SavestateManager::ListStates() const {
    std::vector<std::string> states;

    try {
        if (!fs::exists(m_SavestatesDir)) {
            return Result<std::vector<std::string>>::Ok(states);
        }

        for (const auto &entry : fs::directory_iterator(m_SavestatesDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                states.push_back(entry.path().stem().string());
            }
        }

        return Result<std::vector<std::string>>::Ok(states);
    } catch (const fs::filesystem_error &e) {
        return Result<std::vector<std::string>>::Error("Failed to list savestates: " + std::string(e.what()));
    }
}

Result<SavestateData> SavestateManager::GetStateInfo(const std::string &name) const {
    // Check cache first
    if (m_CacheValid && m_StateCache.count(name) > 0) {
        return Result<SavestateData>::Ok(m_StateCache.at(name));
    }

    if (!StateExists(name)) {
        return Result<SavestateData>::Error("Savestate does not exist: " + name);
    }

    std::string filepath = GetSavestatePath(name);
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return Result<SavestateData>::Error("Failed to open file: " + filepath);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string json = buffer.str();
        file.close();

        auto data_result = SavestateData::FromJson(json);
        if (data_result.IsError()) {
            return Result<SavestateData>::Error("Failed to parse savestate: " + data_result.GetError().message);
        }

        const SavestateData &data = data_result.Unwrap();

        m_StateCache[name] = data;

        return Result<SavestateData>::Ok(data);
    } catch (const std::exception &e) {
        return Result<SavestateData>::Error("Failed to read savestate: " + std::string(e.what()));
    }
}

std::string SavestateManager::GetSavestatesDirectory() const {
    return m_SavestatesDir;
}

// ============================================================================
// Private Methods
// ============================================================================

Result<SavestateData> SavestateManager::CaptureState() const {
    auto *objectProvider = m_Services.Resolve<IObjectProvider>();
    auto *gameStateProvider = m_Services.Resolve<IGameStateProvider>();
    auto *gameQuery = m_Services.Resolve<IGameQuery>();

    if (!objectProvider || !gameStateProvider || !gameQuery) {
        return Result<SavestateData>::Error("Required interfaces not available");
    }

    SavestateData data;

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    data.timestamp = ss.str();

    // Get level info
    data.levelName = gameQuery->GetMapName();
    data.levelNumber = gameStateProvider->GetCurrentLevel();

    // Get ball entity
    CK3dEntity *ball = objectProvider->GetActiveBall();
    if (!ball) {
        return Result<SavestateData>::Error("Ball entity not available");
    }

    // Capture physics state
    VxVector pos;
    ball->GetPosition(&pos);
    data.position = pos;

    // Get physics object for velocity
    auto physics_obj = objectProvider->GetPhysicsObject(ball);
    if (physics_obj) {
        VxVector vel, angVel;
        physics_obj->GetVelocity(&vel, &angVel);
        data.velocity = vel;
        data.angularVelocity = angVel;
    } else {
        data.velocity = VxVector(0, 0, 0);
        data.angularVelocity = VxVector(0, 0, 0);
    }

    // Get rotation
    VxQuaternion rot;
    ball->GetQuaternion(&rot);
    data.rotation = rot;

    // Capture game state
    data.points = gameStateProvider->GetPoints();
    data.lives = gameStateProvider->GetLifeCount();
    data.sector = gameStateProvider->GetCurrentSector();
    data.srScore = gameStateProvider->GetSRScore();
    data.hsScore = static_cast<float>(gameStateProvider->GetHSScore());

    // Capture time state (TODO: implement GetCurrentTick in GameInterface)
    data.tick = 0; // Placeholder until GetCurrentTick is implemented

    return Result<SavestateData>::Ok(data);
}

Result<void> SavestateManager::RestoreState(const SavestateData &data) {
    auto *objectProvider = m_Services.Resolve<IObjectProvider>();
    auto *gameStateProvider = m_Services.Resolve<IGameStateProvider>();
    auto *gameQuery = m_Services.Resolve<IGameQuery>();

    if (!objectProvider || !gameStateProvider || !gameQuery) {
        return Result<void>::Error("Required interfaces not available");
    }

    // Verify we're in the same level
    if (data.levelName != gameQuery->GetMapName()) {
        return Result<void>::Error("Cannot load savestate from different level");
    }

    // Get ball entity
    CK3dEntity *ball = objectProvider->GetActiveBall();
    if (!ball) {
        return Result<void>::Error("Ball entity not available");
    }

    // Restore physics state
    ball->SetPosition(&data.position);
    ball->SetQuaternion(&data.rotation);

    // Restore velocity
    auto physics_obj = objectProvider->GetPhysicsObject(ball);
    if (physics_obj) {
        VxVector linearVel = data.velocity;
        VxVector angularVel = data.angularVelocity;
        physics_obj->SetVelocity(&linearVel, &angularVel);
    }

    if (!gameStateProvider->SetPoints(data.points)) {
        return Result<void>::Error("Failed to restore points");
    }

    if (!gameStateProvider->SetLifeCount(data.lives)) {
        return Result<void>::Error("Failed to restore life count");
    }

    if (!gameStateProvider->SetCurrentSector(data.sector)) {
        return Result<void>::Error("Failed to restore current sector");
    }

    if (!gameStateProvider->SetSRScore(data.srScore)) {
        return Result<void>::Error("Failed to restore SR score");
    }

    if (!gameStateProvider->SetHSScore(static_cast<int>(data.hsScore))) {
        return Result<void>::Error("Failed to restore HS score");
    }

    return Result<void>::Ok();
}

std::string SavestateManager::GetSavestatePath(const std::string &name) const {
    return (fs::path(m_SavestatesDir) / (name + ".json")).string();
}

Result<void> SavestateManager::ValidateName(const std::string &name) const {
    if (name.empty()) {
        return Result<void>::Error("Savestate name cannot be empty");
    }

    // Check for invalid characters (only allow alphanumeric, underscore, hyphen)
    std::regex valid_pattern("^[a-zA-Z0-9_-]+$");
    if (!std::regex_match(name, valid_pattern)) {
        return Result<void>::Error("Savestate name contains invalid characters (only a-zA-Z0-9_- allowed)");
    }

    if (name.length() > 64) {
        return Result<void>::Error("Savestate name too long (max 64 characters)");
    }

    return Result<void>::Ok();
}

