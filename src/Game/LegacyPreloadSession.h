#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// One session belongs to one mod/context lifetime. Collect the actual startup
// owners; never infer a body count, a frame number, or a successful RNG value.
class LegacyPreloadSession {
public:
    struct Owner { uint32_t id; const void *identity; bool retired = false; };
    static bool IsFragment(const char *name) {
        if (!name) return false;
        for (const char *prefix : {"Ball_Paper_piece", "Ball_Stone_piece", "Ball_Wood_piece"}) {
            const auto size = std::strlen(prefix);
            if (std::strncmp(name, prefix, size)) continue;
            const char *number = name + size;
            if (!*number) return false;
            for (; *number; ++number) if (*number < '0' || *number > '9') return false;
            return true;
        }
        return false;
    }
    void Register(uint32_t id, const void *identity, const char *name) {
        if (m_Closed || !m_Error.empty() || !IsFragment(name)) return;
        if (m_Attempted || !id || !identity) { Fail("Unexpected preload object lifecycle."); return; }
        for (const auto &owner : m_Owners)
            if (owner.id == id || owner.identity == identity) { Fail("Duplicate preload object."); return; }
        m_Owners.push_back({id, identity});
    }
    template<class Complete> void Retire(uint32_t id, const void *identity, Complete complete) {
        if (m_Closed || !m_Error.empty()) return;
        Owner *selected = nullptr;
        for (auto &owner : m_Owners) if (owner.id == id) { selected = &owner; break; }
        if (!selected) return;
        if (selected->identity != identity || selected->retired) { Fail("Preload object identity changed."); return; }
        if (!m_Attempted) {
            m_Attempted = true;
            const std::string error = complete(m_Owners);
            if (!error.empty()) { Fail(error); return; }
        }
        selected->retired = true;
    }
    void FinishMenu() {
        if (m_Closed) return;
        m_Closed = true;
        if (!m_Error.empty()) return;
        if (!m_Attempted || m_Owners.empty()) { Fail("Startup preload was not observed. Restart with the original ball resources."); return; }
        for (const auto &owner : m_Owners) if (!owner.retired) { Fail("Startup preload did not finish retiring its objects."); return; }
        m_Ready = true;
    }
    std::string Error() const {
        if (!m_Error.empty()) return "TAS initialization failed: " + m_Error;
        return m_Ready ? std::string() : "TAS initialization is still loading.";
    }
private:
    void Fail(const std::string &reason) { m_Error = reason; m_Ready = false; }
    std::vector<Owner> m_Owners;
    std::string m_Error;
    bool m_Attempted = false;
    bool m_Closed = false;
    bool m_Ready = false;
};
