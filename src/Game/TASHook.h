#pragma once

#include <functional>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <mutex>
#include "Hook.h"
#endif

#include "CKTimeManager.h"
#include "CKInputManager.h"

#ifdef _WIN32
template <typename T>
T ForceReinterpretCast(void *base, size_t offset) {
    void *p = static_cast<char *>(base) + offset;
    return *reinterpret_cast<T *>(&p);
}

template <typename Class>
class PreProcessHook : public Class {
public:
    using MethodType = decltype(&Class::PreProcess);
    using HookType = Hook<MethodType>;
    using InterceptorType = HookInterceptor<MethodType>;
    using CallbackType = typename InterceptorType::CallbackType;

    static bool Enable(CKBaseManager *manager) {
        static std::once_flag flag;
        std::call_once(flag, [&]() {
            // Extract function pointer from vtable
            void **vtable = *reinterpret_cast<void ***>(manager);
            auto preProcessFunc = *reinterpret_cast<MethodType *>(&vtable[5]);

            // Enable hook and interceptor
            if (s_PreProcessHook.Enable(preProcessFunc, &PreProcessHook::PreProcessDetour)) {
                s_PreProcessInterceptor = new HookInterceptor<MethodType>(s_PreProcessHook);
            }
        });

        return s_PreProcessHook.GetOriginal() != nullptr;
    }

    static void Disable() {
        if (s_PreProcessHook.GetOriginal()) {
            s_PreProcessHook.Disable();
            delete s_PreProcessInterceptor;
            s_PreProcessInterceptor = nullptr;
        }
    }

    // Get the original (unhooked) function pointer
    static MethodType GetOriginal() {
        return s_PreProcessHook.GetOriginal();
    }

    // Set a pre-execution callback
    static void AddPreCallback(CallbackType callback) {
        if (s_PreProcessInterceptor) {
            s_PreProcessInterceptor->AddPreCallback(std::move(callback));
        }
    }

    // Remove all pre-execution callbacks
    static void ClearPreCallbacks() {
        if (s_PreProcessInterceptor) {
            s_PreProcessInterceptor->ClearPreCallbacks();
        }
    }

    // Set a post-execution callback
    static void AddPostCallback(CallbackType callback) {
        if (s_PreProcessInterceptor) {
            s_PreProcessInterceptor->AddPostCallback(std::move(callback));
        }
    }

    // Remove all post-execution callbacks
    static void ClearPostCallbacks() {
        if (s_PreProcessInterceptor) {
            s_PreProcessInterceptor->ClearPostCallbacks();
        }
    }

private:
    CKERROR PreProcessDetour() {
        if (s_PreProcessInterceptor) {
            return s_PreProcessInterceptor->InvokeMethod(this);
        }
        return s_PreProcessHook.InvokeMethodOriginal(this);
    }

    static HookType s_PreProcessHook;
    static HookInterceptor<MethodType> *s_PreProcessInterceptor;
};

template <typename Class>
typename PreProcessHook<Class>::HookType PreProcessHook<Class>::s_PreProcessHook;

template <typename Class>
HookInterceptor<typename PreProcessHook<Class>::MethodType> *PreProcessHook<Class>::s_PreProcessInterceptor = nullptr;
#else
// Native arm64 build: patch only the manager instance's vptr.  This preserves
// the exact pre/post-PreProcess timing of the Windows MinHook implementation,
// while chaining cleanly after BML's own shared-vtable hook.
template <typename Class>
class PreProcessHook {
public:
    using CallbackType = std::function<void(Class *)>;
    using OriginalType = CKERROR (*)(Class *);

    static bool Enable(CKBaseManager *base) {
        if (s_Manager)
            return s_Manager == base;
        if (!base)
            return false;

        auto *manager = static_cast<Class *>(base);
        void **originalVTable = *reinterpret_cast<void ***>(manager);
        if (!originalVTable || !originalVTable[kPreProcessSlot])
            return false;

        s_Clone = std::make_unique<void *[]>(kVTableSlots);
        for (size_t i = 0; i < kVTableSlots; ++i)
            s_Clone[i] = originalVTable[i];
        s_Original = reinterpret_cast<OriginalType>(originalVTable[kPreProcessSlot]);
        s_Clone[kPreProcessSlot] = reinterpret_cast<void *>(&Detour);
        s_Manager = manager;
        s_OriginalVTable = originalVTable;
        *reinterpret_cast<void ***>(manager) = s_Clone.get();
        return true;
    }

    static void Disable() {
        if (s_Manager && *reinterpret_cast<void ***>(s_Manager) == s_Clone.get())
            *reinterpret_cast<void ***>(s_Manager) = s_OriginalVTable;
        s_Manager = nullptr;
        s_OriginalVTable = nullptr;
        s_Original = nullptr;
        s_PreCallbacks.clear();
        s_PostCallbacks.clear();
        s_Clone.reset();
    }

    static OriginalType GetOriginal() { return s_Original; }
    static void AddPreCallback(CallbackType callback) { s_PreCallbacks.push_back(std::move(callback)); }
    static void ClearPreCallbacks() { s_PreCallbacks.clear(); }
    static void AddPostCallback(CallbackType callback) { s_PostCallbacks.push_back(std::move(callback)); }
    static void ClearPostCallbacks() { s_PostCallbacks.clear(); }

private:
    static CKERROR Detour(Class *manager) {
        for (auto &callback : s_PreCallbacks)
            callback(manager);
        CKERROR result = s_Original ? s_Original(manager) : CKERR_NOTINITIALIZED;
        for (auto &callback : s_PostCallbacks)
            callback(manager);
        return result;
    }

    // Itanium ABI emits both the complete and deleting destructor entries.
    // Windows/MSVC has one destructor slot, which is why the original hook used
    // index 5.  On arm64 macOS PreProcess is index 6; index 5 is PostClearAll and
    // appeared to hook successfully while never dispatching per-frame callbacks.
    static constexpr size_t kPreProcessSlot = 6;
    // CKInputManager is the largest manager used here (base slots plus its
    // device methods).  Keep the complete dispatch surface on the clone.
    static constexpr size_t kVTableSlots = 66;
    inline static Class *s_Manager = nullptr;
    inline static void **s_OriginalVTable = nullptr;
    inline static OriginalType s_Original = nullptr;
    inline static std::unique_ptr<void *[]> s_Clone;
    inline static std::vector<CallbackType> s_PreCallbacks;
    inline static std::vector<CallbackType> s_PostCallbacks;
};
#endif

using CKTimeManagerHook = PreProcessHook<CKTimeManager>;
using CKInputManagerHook = PreProcessHook<CKInputManager>;
