#pragma once

#include <cstdint>

class CKBaseManager;

/**
 * @brief IVP physics determinism support for physics_RT.dll.
 *
 * - PhysicsLevelBaseline: state restored by canonical TAS level loads (see TASEngine::Start).
 * - FPU monitor: the shipped physics_RT.dll does not set FPU precision in
 *   event_loop(), so physics runs at whatever x87 precision the process is in.
 *   While a TAS is active the monitor logs control word changes seen at the
 *   physics step. It never modifies the control word.
 */
namespace PhysicsDeterminism {
    /**
     * Physics state that survives leaving a level. The IVP environment is not
     * rebuilt between levels, so a later level load inherits the previous
     * level's time factor (the level script raises it from 1x to 2x during
     * loading), freeze-check countdown and seed. That shifts the PSI phase and
     * made re-entered levels replay one frame late.
     */
    struct PhysicsLevelBaseline {
        float timeFactor = 0.0f;       // CKIpionManager::m_PhysicsTimeFactor
        short nextMovementCheck = 0;   // IVP_Environment::next_movement_check
        int32_t randomSeed = 0;        // IVP_RAND_SEED
    };

    /// Hook CKIpionManager::PostProcess. Call once after MinHook is initialized.
    bool EnableFpuMonitor(CKBaseManager *ipionManager);
    void DisableFpuMonitor();

    /// Log x87 control word changes at the physics step while a TAS is active.
    void SetFpuMonitoring(bool enabled);
}
