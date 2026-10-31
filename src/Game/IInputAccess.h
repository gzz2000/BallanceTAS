#pragma once

#include <BML/InputHook.h>

class CKInputManager;

class IInputAccess {
public:
    virtual ~IInputAccess() = default;

    // TAS playback writes the engine's authoritative 256-byte keyboard state.
    // Do not require BML's optional InputHook wrapper: native Ballanced leaves
    // that legacy vtable hook disabled and exposes CKInputManager directly.
    virtual CKInputManager *GetInputManager() const = 0;
};
