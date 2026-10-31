#include "HarnessRuntime.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>
#include <sstream>

#include "HarnessProtocol.h"
#include "Logger.h"
#include "ProjectManager.h"
#include "ScriptContext.h"
#include "ScriptContextManager.h"
#include "ServiceContainer.h"
#include "StartupProjectManager.h"
#include "TASEngine.h"

namespace {
std::string GetEnvironment(const char *name) {
#ifdef _WIN32
    const DWORD length = ::GetEnvironmentVariableA(name, nullptr, 0);
    if (length == 0) return {};

    std::string value(length, '\0');
    const DWORD written = ::GetEnvironmentVariableA(name, value.data(), length);
    if (written == 0 || written >= length) return {};
    value.resize(written);
    return value;
#else
    const char *value = std::getenv(name);
    return value ? value : "";
#endif
}

bool ReadTextFile(const std::string &path, std::string &text) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream stream;
    stream << input.rdbuf();
    text = stream.str();
    return input.good() || input.eof();
}
}

HarnessRuntime::HarnessRuntime(TASEngine *engine) : m_Engine(engine) {}

bool HarnessRuntime::IsRequested() {
    return !GetEnvironment("BALLANCE_HARNESS_REQUEST").empty() ||
           !GetEnvironment("BALLANCE_HARNESS_RESULT").empty();
}

bool HarnessRuntime::Initialize() {
    if (!IsRequested()) return true;

    m_RequestPath = GetEnvironment("BALLANCE_HARNESS_REQUEST");
    m_ResultPath = GetEnvironment("BALLANCE_HARNESS_RESULT");
    m_Active = true;

    if (m_RequestPath.empty() || m_ResultPath.empty()) {
        Log::Error("Harness requires BALLANCE_HARNESS_REQUEST and BALLANCE_HARNESS_RESULT.");
        return false;
    }

    std::string json;
    if (!ReadTextFile(m_RequestPath, json)) {
        Complete(false, "cannot read harness request: " + m_RequestPath);
        return true;
    }

    HarnessRequest request;
    std::string error;
    if (!ParseHarnessRequest(json, request, error)) {
        Complete(false, error);
        return true;
    }

    m_Project = request.project;
    m_MaxTicks = request.maxTicks;

    auto &services = m_Engine->GetServiceProvider();
    auto *projects = services.Resolve<ProjectManager>();
    auto *startup = services.Resolve<StartupProjectManager>();
    if (!projects || !startup) {
        Complete(false, "project services are unavailable");
        return true;
    }

    projects->RefreshProjects();
    startup->SetStartupEnabled(true);
    startup->SetAutoLoadEnabled(true);
    if (!startup->SetStartupProject(m_Project)) {
        Complete(false, "harness project not found or invalid: " + m_Project);
        return true;
    }
    if (!startup->LoadAndExecuteStartupScript()) {
        Complete(false, "failed to start harness project: " + m_Project);
        return true;
    }

    m_Started = true;
    m_StartTick = m_Engine->GetCurrentTick();
    Log::Info("Harness started project '%s' (max ticks: %zu).", m_Project.c_str(), m_MaxTicks);
    return true;
}

void HarnessRuntime::Tick() {
    if (!m_Active || !m_Started || m_Completed) return;

    const size_t currentTick = m_Engine->GetCurrentTick();
    if (currentTick - m_StartTick >= m_MaxTicks) {
        Complete(false, "harness exceeded max_ticks");
        return;
    }

    auto *contexts = m_Engine->GetServiceProvider().Resolve<ScriptContextManager>();
    auto context = contexts ? contexts->GetContext(ScriptContextManager::GlobalContextName()) : nullptr;
    if (!context || !context->IsExecuting()) {
        Complete(false, "harness project ended without tas.harness.pass/fail");
    }
}

bool HarnessRuntime::Complete(bool passed, const std::string &message) {
    if (!m_Active || m_Completed) return false;
    m_Completed = WriteResult(passed, message);
    if (m_Completed) {
        Log::Info("Harness %s: %s", passed ? "PASS" : "FAIL", message.c_str());
    }
    return m_Completed;
}

bool HarnessRuntime::WriteResult(bool passed, const std::string &message) {
    const size_t tick = m_Engine ? m_Engine->GetCurrentTick() : 0;
    const std::string json = BuildHarnessResult(passed, m_Project, message, tick);
    if (json.empty()) {
        Log::Error("Failed to serialize harness result.");
        return false;
    }

#ifdef _WIN32
    const auto processId = ::GetCurrentProcessId();
#else
    const auto processId = ::getpid();
#endif
    const std::string temporary = m_ResultPath + ".tmp." + std::to_string(processId);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            Log::Error("Failed to open harness result temporary file: %s", temporary.c_str());
            return false;
        }
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        output.flush();
        if (!output) {
            output.close();
            std::error_code removeError;
            std::filesystem::remove(temporary, removeError);
            return false;
        }
    }

#ifdef _WIN32
    const bool published = ::MoveFileExA(temporary.c_str(), m_ResultPath.c_str(),
                                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    std::error_code publishError;
    std::filesystem::rename(temporary, m_ResultPath, publishError);
    const bool published = !publishError;
#endif
    if (!published) {
#ifdef _WIN32
        Log::Error("Failed to publish harness result: %lu", ::GetLastError());
#else
        Log::Error("Failed to publish harness result: %s", publishError.message().c_str());
#endif
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return false;
    }
    return true;
}
