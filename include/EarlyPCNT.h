#ifndef EARLY_PCNT_H
#define EARLY_PCNT_H

#include <driver/pulse_cnt.h>

// ============================================================================
// Early PCNT — lightweight standalone pulse counter for boot-time detection.
// Initialized as early as possible (GCC constructor, before app_main).
// Ownership is transferred to TrafficMonitor via detach() — no teardown needed.
// ============================================================================

struct PcntHandles {
    pcnt_unit_handle_t    unit;
    pcnt_channel_handle_t channel;
};

namespace EarlyPCNT {
    void init(int gpio);     // Create PCNT unit on gpio, start counting immediately
    int  read();             // Read accumulated pulse count (non-destructive)
    void teardown();         // Delete PCNT unit to free resources (only if NOT detached)
    PcntHandles detach();    // Transfer ownership — caller now owns the unit+channel
}

#endif // EARLY_PCNT_H
