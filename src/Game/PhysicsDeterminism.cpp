#include "PhysicsDeterminism.h"

#include <cstdlib>

#ifdef _WIN32
#include <float.h>
#endif

#include "CKBaseManager.h"

#ifdef _WIN32
#include "Hook.h"
#endif
#include "Logger.h"

namespace {
    bool g_Monitoring = false;
#ifdef _WIN32
    constexpr unsigned int kFpuMask = _MCW_PC | _MCW_RC;
    constexpr uint32_t kMaxLoggedChanges = 20;

    bool g_HasLastSeen = false;
    unsigned int g_LastSeen = 0;
    uint32_t g_Changes = 0;

    unsigned int ReadX87Control() {
        unsigned int x87 = 0;
        __control87_2(0, 0, &x87, nullptr);
        return x87 & kFpuMask;
    }

    class IpionPostProcessHook : public CKBaseManager {
    public:
        using MethodType = decltype(&CKBaseManager::PostProcess);

        CKERROR Detour() {
            if (g_Monitoring) {
                const unsigned int current = ReadX87Control();
                if (!g_HasLastSeen) {
                    g_LastSeen = current;
                    g_HasLastSeen = true;
                    Log::Info("Physics FPU control word at TAS start: 0x%X", current);
                } else if (current != g_LastSeen) {
                    ++g_Changes;
                    if (g_Changes <= kMaxLoggedChanges) {
                        Log::Warn("Physics FPU control word changed 0x%X -> 0x%X before physics step.",
                                  g_LastSeen, current);
                    }
                    g_LastSeen = current;
                }
            }
            return s_Hook.InvokeMethodOriginal(this);
        }

        static Hook<MethodType> s_Hook;
    };

    Hook<IpionPostProcessHook::MethodType> IpionPostProcessHook::s_Hook;
#endif
}

namespace PhysicsDeterminism {
    bool EnableFpuMonitor(CKBaseManager *ipionManager) {
#ifndef _WIN32
        (void) ipionManager;
        // Apple Silicon has no x87 control word.  Its FP environment is
        // architectural state with a different contract, so this Windows-only
        // diagnostic has no native equivalent and is intentionally a no-op.
        return true;
#else
        if (!ipionManager) return false;
        if (IpionPostProcessHook::s_Hook.GetOriginal()) return true;

        // The monitor is diagnostic only.  In particular, do not install it in
        // normal replay builds: intercepting a C++ virtual member across the
        // MSVC/clang-cl/Wine ABI boundary can itself perturb or crash the first
        // physics step, defeating the determinism experiment it is meant to
        // observe.  It can still be enabled explicitly for dedicated x87
        // investigations.
        const char *enableMonitor = std::getenv("BALLANCE_TAS_FPU_MONITOR");
        if (!enableMonitor || enableMonitor[0] != '1' || enableMonitor[1] != '\0') {
            return true;
        }

        // CKBaseManager vtable: PreProcess is slot 5, PostProcess slot 6
        // (verified against ??_7CKIpionManager@@6B@ in physics_RT.dll).
        void **vtable = *reinterpret_cast<void ***>(ipionManager);
        auto postProcess = *reinterpret_cast<IpionPostProcessHook::MethodType *>(&vtable[6]);
        return IpionPostProcessHook::s_Hook.Enable(postProcess, &IpionPostProcessHook::Detour);
#endif
    }

    void DisableFpuMonitor() {
#ifdef _WIN32
        IpionPostProcessHook::s_Hook.Disable();
#endif
        g_Monitoring = false;
    }

    void SetFpuMonitoring(bool enabled) {
#ifndef _WIN32
        g_Monitoring = enabled;
        return;
#else
        if (g_Monitoring && !enabled) {
            if (g_Changes > 0) {
                Log::Info("Physics FPU control word changed %u time(s) during the TAS.", g_Changes);
            }
            g_HasLastSeen = false;
            g_Changes = 0;
        }
        g_Monitoring = enabled;
#endif
    }
}
