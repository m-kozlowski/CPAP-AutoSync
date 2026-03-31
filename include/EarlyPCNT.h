#ifndef EARLY_PCNT_H
#define EARLY_PCNT_H

#include <driver/pulse_cnt.h>

// ============================================================================
// Early PCNT — lightweight standalone pulse counter for boot-time detection.
// Uses its own PCNT unit, separate from TrafficMonitor.
// Must be initialized as early as possible in setup(), torn down before
// TrafficMonitor.begin() is called.
// ============================================================================
namespace EarlyPCNT {
    void init(int gpio);     // Create PCNT unit on gpio, start counting immediately
    int  read();             // Read accumulated pulse count (non-destructive)
    void teardown();         // Delete PCNT unit to free resources
}

#endif // EARLY_PCNT_H
