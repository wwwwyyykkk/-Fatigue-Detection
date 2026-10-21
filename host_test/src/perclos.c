#include "perclos.h"

int perclos_level(float ratio)
{
    if (ratio < 0.0f || ratio > 1.0f) {
        return -1;
    }
    if (ratio <= 0.15f) {
        return FATIGUE_NORMAL;
    }
    if (ratio <= 0.30f) {
        return FATIGUE_MILD;
    }
    return FATIGUE_SEVERE;
}
