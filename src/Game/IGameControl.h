#pragma once

#include <cstddef>
#include <functional>
#include <string>

enum class UIMode;

class IGameControl {
public:
    virtual ~IGameControl() = default;
    // Native legacy records depend on a qualified preload initial state.
    // Existing Windows hosts and non-native test hosts keep their own contract.
    virtual std::string GetLegacyPreloadError() const { return {}; }

    virtual void AcquireGameplayInfo() = 0;
    virtual void AcquireKeyBindings() = 0;
    virtual void ResetPhysicsTime() = 0;
    virtual void SetUIMode(UIMode mode) = 0;
    virtual void AddTimer(size_t tick, const std::function<void()> &callback) = 0;
};
