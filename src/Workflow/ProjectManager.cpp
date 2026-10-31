#include "ProjectManager.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <stdexcept>

#include <zip.h>

#include "Logger.h"
#include "LuaRuntime/LuaProtectedCall.h"
#include "TASEngine.h"

namespace fs = std::filesystem;

ProjectManager::ProjectManager(TASEngine *engine) : m_Engine(engine) {
    if (!m_Engine) {
        throw std::runtime_error("ProjectManager requires a valid TASEngine instance.");
    }

    // Initialize dedicated Lua state for manifest parsing
    m_ManifestState.OpenStandardLibraries();

    // Define the root directory for all TAS projects.
    m_TASRootPath = m_Engine->GetPath();

    // Create temp directory for zip extractions
    m_TempDir = (fs::path(m_TASRootPath) / "temp").string();

    // Ensure the directories exist.
    if (!fs::exists(m_TASRootPath)) {
        fs::create_directories(m_TASRootPath);
    }
    if (!fs::exists(m_TempDir)) {
        fs::create_directories(m_TempDir);
    }

    RefreshProjects();
}

ProjectManager::~ProjectManager() {
    CleanupTempDirectories();
}

// Identifies one version of a project on disk: the manifest of a directory project,
// otherwise the file itself.
static std::string ProjectStamp(const fs::directory_entry &entry) {
    std::error_code ec;
    const fs::path path = entry.is_directory(ec) ? entry.path() / "manifest.lua" : entry.path();
    const auto time = fs::last_write_time(path, ec);
    if (ec) {
        return {};
    }
    const auto size = fs::is_regular_file(path, ec) ? fs::file_size(path, ec) : 0;
    return std::to_string(static_cast<long long>(time.time_since_epoch().count())) + ":" +
           std::to_string(static_cast<unsigned long long>(ec ? 0 : size));
}

std::string ProjectManager::ScanSignature() const {
    std::string signature;
    std::error_code ec;
    for (fs::directory_iterator it(m_TASRootPath, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename() == "temp") {
            continue;
        }
        signature += it->path().filename().string();
        signature += '|';
        signature += ProjectStamp(*it);
        signature += '\n';
    }
    return signature;
}

bool ProjectManager::RefreshIfChanged() {
    if (ScanSignature() == m_ScanSignature) {
        return false;
    }
    RefreshProjects();
    return true;
}

void ProjectManager::RefreshProjects() {
    // Leftovers from earlier runs. Later rescans keep extracted projects: they may be running.
    if (!m_HasScanned) {
        CleanupTempDirectories();
        m_HasScanned = true;
    }
    m_ScanSignature = ScanSignature();

    // Unchanged projects keep their objects, so pointers held by playback, script contexts,
    // the runtime session or the Player host bridge stay valid and nothing is parsed twice.
    std::vector<std::unique_ptr<TASProject>> previous = std::move(m_Projects);
    m_Projects.clear();
    auto reuse = [&](const std::string &path, const std::string &stamp) -> bool {
        auto it = std::find_if(previous.begin(), previous.end(),
                               [&](const auto &p) { return p->GetPath() == path; });
        if (it == previous.end()) {
            return false;
        }
        // The current project is never swapped out from under its users.
        if (it->get() != m_CurrentProject && m_ProjectStamps[it->get()] != stamp) {
            return false;
        }
        m_Projects.push_back(std::move(*it));
        previous.erase(it);
        return true;
    };
    auto add = [&](std::unique_ptr<TASProject> project, const std::string &stamp) {
        if (project && project->IsValid()) {
            m_ProjectStamps[project.get()] = stamp;
            m_Projects.push_back(std::move(project));
        }
    };

    Log::Info("Scanning for TAS projects in: %s", m_TASRootPath.c_str());

    try {
        for (const auto &entry : fs::directory_iterator(m_TASRootPath)) {
            const std::string stamp = ProjectStamp(entry);
            if (entry.is_directory()) {
                // Traditional directory-based script project
                std::string projectPath = NormalizePath(entry.path().string());
                std::string manifestPath = (fs::path(projectPath) / "manifest.lua").string();
                if (fs::exists(manifestPath) && ValidateProjectStructure(projectPath) &&
                    !reuse(projectPath, stamp)) {
                    add(LoadDirectoryProject(projectPath), stamp);
                }
            } else if (entry.is_regular_file()) {
                std::string filePath = NormalizePath(entry.path().string());
                std::string extension = entry.path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(), tolower);

                if (extension == ".zip") {
                    // Zip-based script project
                    if (reuse(filePath, stamp)) {
                        continue;
                    }
                    if (ValidateZipProject(filePath)) {
                        add(LoadZipProject(filePath), stamp);
                    } else {
                        Log::Warn("Invalid zip project structure: %s", filePath.c_str());
                    }
                } else if (extension == ".tas") {
                    // Binary record project
                    if (!reuse(filePath, stamp)) {
                        add(LoadRecordProject(filePath), stamp);
                    }
                }
            }
        }
    } catch (const fs::filesystem_error &e) {
        Log::Error("Filesystem error while scanning for projects: %s", e.what());
    }

    // Projects that changed or disappeared stay alive until shutdown: something may still
    // point at them. The current project stays listed even when its files are gone.
    for (auto &project : previous) {
        if (project.get() == m_CurrentProject) {
            m_Projects.push_back(std::move(project));
        } else {
            m_RetiredProjects.push_back(std::move(project));
        }
    }

    // Sort projects alphabetically by name for consistent UI display.
    std::sort(m_Projects.begin(), m_Projects.end(), [](const auto &a, const auto &b) {
        return a->GetName() < b->GetName();
    });
    ++m_Generation;

    int directoryProjects = 0;
    int zipProjects = 0;
    int recordProjects = 0;
    for (const auto &project : m_Projects) {
        if (project->IsRecordProject()) {
            recordProjects++;
        } else if (project->IsZipProject()) {
            zipProjects++;
        } else {
            directoryProjects++;
        }
    }
    Log::Info("Found %d valid TAS projects (%d directories, %d zip files, %d record files).",
                                static_cast<int>(m_Projects.size()), directoryProjects, zipProjects, recordProjects);
}

TASProject *ProjectManager::FindProject(const std::string &name) const {
    for (const auto &project : m_Projects) {
        if (project && project->GetName() == name) {
            return project.get();
        }
    }
    return nullptr;
}

TASProject *ProjectManager::ResolveLaunchTarget(const std::string &target, std::string &error) {
    if (target.empty()) {
        error = "TAS launch target is empty";
        return nullptr;
    }

    if (TASProject *project = FindProject(target)) {
        return project;
    }

    std::error_code ec;
    const fs::path path = fs::absolute(fs::path(target), ec);
    if (ec || !fs::exists(path, ec)) {
        error = "TAS project name or path was not found: " + target;
        return nullptr;
    }

    std::unique_ptr<TASProject> project;
    const std::string normalized = NormalizePath(path.string());
    if (fs::is_directory(path, ec)) {
        if (!ValidateProjectStructure(normalized)) {
            error = "TAS directory is missing a valid manifest.lua/entry script: " + normalized;
            return nullptr;
        }
        project = LoadDirectoryProject(normalized);
    } else if (fs::is_regular_file(path, ec)) {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".zip") {
            if (!ValidateZipProject(normalized)) {
                error = "TAS zip project is invalid: " + normalized;
                return nullptr;
            }
            project = LoadZipProject(normalized);
        } else if (extension == ".tas") {
            project = LoadRecordProject(normalized);
        } else {
            error = "Unsupported TAS target type: " + normalized;
            return nullptr;
        }
    }

    if (!project || !project->IsValid()) {
        error = "TAS target failed validation: " + normalized;
        return nullptr;
    }

    TASProject *resolved = project.get();
    m_Projects.push_back(std::move(project));
    return resolved;
}

std::unique_ptr<TASProject> ProjectManager::LoadDirectoryProject(const std::string &projectPath) {
    std::string manifestPath = (fs::path(projectPath) / "manifest.lua").string();
    tas::lua::LuaValue manifest_table = ParseManifestFile(manifestPath);

    if (!manifest_table.IsTable()) {
        Log::Warn("Could not parse manifest for directory project at: %s", projectPath.c_str());
        return nullptr;
    }

    auto project = std::make_unique<TASProject>(projectPath, manifest_table);
    project->SetIsZipProject(false);

    return project;
}

std::unique_ptr<TASProject> ProjectManager::LoadZipProject(const std::string &zipPath) {
    // Read manifest from zip
    std::string manifestContent;
    if (!ReadFileFromZip(zipPath, "manifest.lua", manifestContent)) {
        Log::Warn("Could not read manifest from zip project: %s", zipPath.c_str());
        return nullptr;
    }

    // Create temporary file for manifest
    std::string tempManifest = m_TempDir + "temp_manifest_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".lua";

    try {
        std::ofstream tempFile(tempManifest);
        if (!tempFile.is_open()) {
            Log::Error("Could not create temporary manifest file: %s", tempManifest.c_str());
            return nullptr;
        }
        tempFile << manifestContent;
        tempFile.close();

        // Parse the temporary manifest
        tas::lua::LuaValue manifest_table = ParseManifestFile(tempManifest);

        // Clean up temporary file
        fs::remove(tempManifest);

        if (!manifest_table.IsTable()) {
            Log::Warn("Could not parse manifest for zip project: %s", zipPath.c_str());
            return nullptr;
        }

        auto project = std::make_unique<TASProject>(zipPath, manifest_table);
        project->SetIsZipProject(true);

        return project;
    } catch (const std::exception &e) {
        // Ensure cleanup even on exceptions
        if (fs::exists(tempManifest)) {
            fs::remove(tempManifest);
        }
        Log::Error("Exception loading zip project %s: %s", zipPath.c_str(), e.what());
        return nullptr;
    }
}

std::unique_ptr<TASProject> ProjectManager::LoadRecordProject(const std::string &recordPath) {
    Log::Info("Loading record project: %s", recordPath.c_str());

    try {
        auto project = std::make_unique<TASProject>(recordPath);

        if (project->IsValid()) {
            Log::Info("Record project loaded: %s (%zu frames)",
                                        project->GetName().c_str(),
                                        project->GetRecordFrameCount());
            return project;
        } else {
            Log::Warn("Invalid record project: %s", recordPath.c_str());
            return nullptr;
        }
    } catch (const std::exception &e) {
        Log::Error("Exception loading record project %s: %s", recordPath.c_str(), e.what());
        return nullptr;
    }
}

std::string ProjectManager::PrepareProjectForExecution(TASProject *project) {
    if (!project) {
        Log::Error("Cannot prepare null project for execution.");
        return "";
    }

    // Record projects don't need preparation - they're single files
    if (project->IsRecordProject()) {
        return project->GetRecordFilePath();
    }

    // For script projects, handle as before...
    // For directory projects, return the path directly
    if (!project->IsZipProject()) {
        return project->GetPath();
    }

    // For zip projects, check if we already have an extracted version
    auto it = m_ProjectTempDirectories.find(project);
    if (it != m_ProjectTempDirectories.end()) {
        // Check if the temp directory still exists and is valid
        if (fs::exists(it->second) && ValidateProjectStructure(it->second)) {
            Log::Info("Using existing extracted project: %s", it->second.c_str());
            return it->second;
        } else {
            // Clean up invalid temp directory
            m_ProjectTempDirectories.erase(it);
        }
    }

    // Extract the zip project to a temporary directory
    std::string projectName = project->GetName();
    // Sanitize project name for directory creation
    std::replace_if(projectName.begin(), projectName.end(),
                    [](char c) { return !std::isalnum(c) && c != '_' && c != '-'; }, '_');

    std::string tempDirPath = CreateTempDirectory("project_" + projectName);

    if (!ExtractZipProject(project->GetPath(), tempDirPath)) {
        Log::Error("Failed to extract zip project for execution: %s", project->GetPath().c_str());
        RemoveDirectory(tempDirPath);
        return "";
    }

    // Validate the extracted project structure
    if (!ValidateProjectStructure(tempDirPath)) {
        Log::Error("Extracted zip project has invalid structure: %s", tempDirPath.c_str());
        RemoveDirectory(tempDirPath);
        return "";
    }

    // Store the temp directory for this project
    m_ProjectTempDirectories[project] = tempDirPath;

    Log::Info("Successfully prepared zip project for execution: %s -> %s",
                                project->GetPath().c_str(), tempDirPath.c_str());

    return tempDirPath;
}

bool ProjectManager::ExtractZipProject(const std::string &zipPath, const std::string &tempDir) {
    Log::Info("Extracting zip project: %s to %s", zipPath.c_str(), tempDir.c_str());

    // Create temporary directory
    try {
        if (!fs::create_directories(tempDir)) {
            if (!fs::exists(tempDir)) {
                Log::Error("Failed to create temp directory: %s", tempDir.c_str());
                return false;
            }
        }
    } catch (const fs::filesystem_error &e) {
        Log::Error("Filesystem error creating temp directory %s: %s", tempDir.c_str(), e.what());
        return false;
    }

    // Extract zip file using the zip library
    struct zip_t *zip = zip_open(zipPath.c_str(), 0, 'r');
    if (!zip) {
        Log::Error("Failed to open zip file: %s", zipPath.c_str());
        RemoveDirectory(tempDir);
        return false;
    }

    bool success = true;
    ssize_t totalEntries = zip_entries_total(zip);

    if (totalEntries <= 0) {
        Log::Error("Zip file appears to be empty or corrupted: %s", zipPath.c_str());
        zip_close(zip);
        RemoveDirectory(tempDir);
        return false;
    }

    Log::Info("Extracting %d entries from zip file...", static_cast<int>(totalEntries));

    for (ssize_t i = 0; i < totalEntries; ++i) {
        if (zip_entry_openbyindex(zip, i) != 0) {
            Log::Error("Failed to open zip entry at index %d", static_cast<int>(i));
            success = false;
            break;
        }

        const char *entryName = zip_entry_name(zip);
        if (!entryName) {
            zip_entry_close(zip);
            continue;
        }

        std::string normalizedEntryName = NormalizePath(entryName);
        std::string outputPath = (fs::path(tempDir) / fs::path(normalizedEntryName)).string();

        // Create parent directories if needed
        fs::path outputFilePath(outputPath);
        try {
            if (outputFilePath.has_parent_path()) {
                fs::create_directories(outputFilePath.parent_path());
            }
        } catch (const fs::filesystem_error &e) {
            Log::Error("Failed to create directory for %s: %s", outputPath.c_str(), e.what());
            zip_entry_close(zip);
            success = false;
            break;
        }

        // Check if this is a directory entry
        if (zip_entry_isdir(zip)) {
            try {
                fs::create_directories(outputPath);
            } catch (const fs::filesystem_error &e) {
                Log::Error("Failed to create directory %s: %s", outputPath.c_str(), e.what());
                success = false;
            }
            zip_entry_close(zip);
            continue;
        }

        // Extract file content
        if (zip_entry_fread(zip, outputPath.c_str()) != 0) {
            Log::Error("Failed to extract file: %s", outputPath.c_str());
            success = false;
            zip_entry_close(zip);
            break;
        }

        zip_entry_close(zip);
    }

    zip_close(zip);

    if (!success) {
        Log::Error("Failed to extract zip project, cleaning up temp directory");
        RemoveDirectory(tempDir);
        return false;
    }

    Log::Info("Successfully extracted zip project to: %s", tempDir.c_str());
    return true;
}

bool ProjectManager::CompressProject(const std::string &projectPath, const std::string &zipPath) {
    Log::Info("Compressing project: %s to %s", projectPath.c_str(), zipPath.c_str());

    if (!ValidateProjectStructure(projectPath)) {
        Log::Error("Project structure validation failed: %s", projectPath.c_str());
        return false;
    }

    std::vector<std::pair<std::string, std::string>> files; // <full_path, relative_path>

    // Collect all files in the project directory
    try {
        for (const auto &entry : fs::recursive_directory_iterator(projectPath)) {
            if (entry.is_regular_file()) {
                std::string fullPath = entry.path().string();
                std::string relativePath = fs::relative(entry.path(), projectPath).string();

                // Convert backslashes to forward slashes for zip compatibility
                std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
                files.emplace_back(fullPath, relativePath);
            }
        }
    } catch (const fs::filesystem_error &e) {
        Log::Error("Error scanning project directory: %s", e.what());
        return false;
    }

    if (files.empty()) {
        Log::Error("No files found in project directory: %s", projectPath.c_str());
        return false;
    }

    // Create zip file
    struct zip_t *zip = zip_open(zipPath.c_str(), ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    if (!zip) {
        Log::Error("Failed to create zip file: %s", zipPath.c_str());
        return false;
    }

    bool success = true;
    for (const auto &[filePath, relativePath] : files) {
        if (zip_entry_open(zip, relativePath.c_str()) != 0) {
            Log::Error("Failed to open zip entry: %s", relativePath.c_str());
            success = false;
            break;
        }

        if (zip_entry_fwrite(zip, filePath.c_str()) != 0) {
            Log::Error("Failed to write file to zip: %s", filePath.c_str());
            success = false;
            zip_entry_close(zip);
            break;
        }

        zip_entry_close(zip);
    }

    zip_close(zip);

    if (success) {
        Log::Info("Successfully compressed project with %d files", static_cast<int>(files.size()));
    } else {
        fs::remove(zipPath); // Clean up failed zip
    }

    return success;
}

bool ProjectManager::ValidateZipProject(const std::string &zipPath) {
    struct zip_t *zip = zip_open(zipPath.c_str(), 0, 'r');
    if (!zip) {
        return false;
    }

    bool hasManifest = false;
    bool hasMainScript = false;

    ssize_t totalEntries = zip_entries_total(zip);
    for (ssize_t i = 0; i < totalEntries; ++i) {
        if (zip_entry_openbyindex(zip, i) != 0) {
            continue;
        }

        const char *entryName = zip_entry_name(zip);
        if (entryName) {
            std::string name = entryName;
            // Handle both forward and back slashes
            if (name == "manifest.lua" || name == ".\\manifest.lua" || name == "./manifest.lua") {
                hasManifest = true;
            } else if (name == "main.lua" || name == ".\\main.lua" || name == "./main.lua") {
                hasMainScript = true;
            }
        }

        zip_entry_close(zip);

        if (hasManifest && hasMainScript) {
            break;
        }
    }

    zip_close(zip);

    bool isValid = hasManifest && hasMainScript;
    if (!isValid) {
        Log::Warn("Zip project validation failed: %s (manifest: %s, main: %s)",
                                    zipPath.c_str(), hasManifest ? "yes" : "no", hasMainScript ? "yes" : "no");
    }

    return isValid;
}

bool ProjectManager::ReadFileFromZip(const std::string &zipPath, const std::string &fileName, std::string &content) {
    struct zip_t *zip = zip_open(zipPath.c_str(), 0, 'r');
    if (!zip) {
        Log::Error("Failed to open zip file: %s", zipPath.c_str());
        return false;
    }

    bool success = false;
    if (zip_entry_open(zip, fileName.c_str()) == 0) {
        void *buf = nullptr;
        size_t bufsize = 0;

        ssize_t result = zip_entry_read(zip, &buf, &bufsize);
        if (result > 0 && buf) {
            content.assign(static_cast<char *>(buf), bufsize);
            success = true;
        }

        if (buf) {
            free(buf);
        }

        zip_entry_close(zip);
    }

    zip_close(zip);

    if (!success) {
        Log::Error("Failed to read file '%s' from zip: %s", fileName.c_str(), zipPath.c_str());
    }

    return success;
}

void ProjectManager::SetCurrentProject(TASProject *project) {
    // Clean up previous project's temp directory if it was different
    if (m_CurrentProject && m_CurrentProject != project) {
        CleanupProjectTempDirectory(m_CurrentProject);
    }

    m_CurrentProject = project;
}

std::string ProjectManager::CreateTempDirectory(const std::string &baseName) {
    // Generate unique directory name
    auto now = std::chrono::steady_clock::now();
    auto timestamp = now.time_since_epoch().count();

    std::string tempDirName = baseName + "_" + std::to_string(timestamp);
    std::string tempDirPath = m_TempDir + tempDirName;

    // Ensure uniqueness
    int counter = 0;
    while (fs::exists(tempDirPath) && counter < 1000) {
        tempDirPath = m_TempDir + tempDirName + "_" + std::to_string(counter);
        counter++;
    }

    return tempDirPath;
}

bool ProjectManager::RemoveDirectory(const std::string &dirPath) {
    try {
        return fs::remove_all(dirPath) > 0;
    } catch (const fs::filesystem_error &e) {
        Log::Error("Failed to remove directory '%s': %s", dirPath.c_str(), e.what());
        return false;
    }
}

void ProjectManager::CleanupTempDirectories() {
    Log::Info("Cleaning up temporary directories");

    // Clean up project-specific temp directories
    for (auto it = m_ProjectTempDirectories.begin(); it != m_ProjectTempDirectories.end();) {
        const std::string &tempDir = it->second;
        if (fs::exists(tempDir)) {
            if (RemoveDirectory(tempDir)) {
                Log::Info("Cleaned up temp directory: %s", tempDir.c_str());
            }
        }
        it = m_ProjectTempDirectories.erase(it);
    }

    // Clean up legacy temp directories
    for (const auto &tempDir : m_TempDirectories) {
        if (fs::exists(tempDir)) {
            RemoveDirectory(tempDir);
        }
    }
    m_TempDirectories.clear();

    // Also clean up any orphaned temp files/directories
    try {
        for (const auto &entry : fs::directory_iterator(m_TempDir)) {
            if (entry.is_directory() || entry.path().filename().string().find("temp_") == 0) {
                fs::remove_all(entry.path());
            }
        }
    } catch (const fs::filesystem_error &e) {
        Log::Warn("Error during temp cleanup: %s", e.what());
    }
}

void ProjectManager::CleanupProjectTempDirectory(TASProject *project) {
    if (!project) return;

    auto it = m_ProjectTempDirectories.find(project);
    if (it != m_ProjectTempDirectories.end()) {
        const std::string &tempDir = it->second;
        if (fs::exists(tempDir)) {
            if (RemoveDirectory(tempDir)) {
                Log::Info("Cleaned up temp directory for project %s: %s",
                                            project->GetName().c_str(), tempDir.c_str());
            }
        }
        m_ProjectTempDirectories.erase(it);
    }
}

std::string ProjectManager::GetProjectNameFromZip(const std::string &zipPath) {
    std::string manifestContent;
    if (ReadFileFromZip(zipPath, "manifest.lua", manifestContent)) {
        // Quick and dirty parsing to extract name
        size_t namePos = manifestContent.find("name = \"");
        if (namePos != std::string::npos) {
            namePos += 8; // Length of "name = \""
            size_t endPos = manifestContent.find("\"", namePos);
            if (endPos != std::string::npos) {
                return manifestContent.substr(namePos, endPos - namePos);
            }
        }
    }

    // Fallback to filename without extension
    fs::path zipFilePath(zipPath);
    return zipFilePath.stem().string();
}

std::string ProjectManager::NormalizePath(const std::string &path) {
    std::string normalized = path;
#ifdef _WIN32
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
#else
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
#endif
    return normalized;
}

bool ProjectManager::ValidateProjectStructure(const std::string &projectPath) {
    if (!fs::exists(projectPath) || !fs::is_directory(projectPath)) {
        return false;
    }

    // Check for required files
    std::string manifestPath = (fs::path(projectPath) / "manifest.lua").string();
    std::string mainScriptPath = (fs::path(projectPath) / "main.lua").string();

    return fs::exists(manifestPath) && fs::exists(mainScriptPath) &&
        fs::is_regular_file(manifestPath) && fs::is_regular_file(mainScriptPath);
}

tas::lua::LuaValue ProjectManager::ParseManifestFile(const std::string &path) {
    lua_State *state = m_ManifestState.Get();
    const int top = lua_gettop(state);

    auto loadResult = m_ManifestState.LoadFile(path);
    if (loadResult.IsError()) {
        lua_settop(state, top);
        Log::Error("Error loading manifest file '%s': %s",
                   path.c_str(), loadResult.GetError().message.c_str());
        return tas::lua::LuaValue();
    }

    auto callResult = tas::lua::ProtectedCall(state, 0, 1);
    if (callResult.IsError()) {
        lua_settop(state, top);
        Log::Error("Error parsing manifest file '%s': %s",
                   path.c_str(), callResult.GetError().message.c_str());
        return tas::lua::LuaValue();
    }

    if (!lua_istable(state, -1)) {
        lua_settop(state, top);
        Log::Warn("Manifest file '%s' did not return a table.", path.c_str());
        return tas::lua::LuaValue();
    }

    auto manifest = tas::lua::LuaValue::FromStack(state, -1);
    lua_settop(state, top);
    if (manifest.IsError()) {
        Log::Error("Error serializing manifest file '%s': %s",
                   path.c_str(), manifest.GetError().message.c_str());
        return tas::lua::LuaValue();
    }
    return std::move(manifest.Unwrap());
}
