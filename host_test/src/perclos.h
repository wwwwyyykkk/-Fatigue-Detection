#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FATIGUE_NORMAL = 0,
    FATIGUE_MILD   = 1,
    FATIGUE_SEVERE = 2
} FatigueLevel;

/**
 * Map a closed-eye ratio (0.0 - 1.0) to a fatigue level.
 *
 * Thresholds match drivesafe/config.h:
 *   <= 0.15 -> normal
 *   <= 0.30 -> mild
 *   >  0.30 -> severe
 *
 * @return FatigueLevel, or -1 when ratio is outside [0.0, 1.0].
 */
int perclos_level(float ratio);

#ifdef __cplusplus
}
#endif
