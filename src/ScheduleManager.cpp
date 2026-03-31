#include "ScheduleManager.h"
#include "Logger.h"
#include <esp_sntp.h>

extern bool g_heapRecoveryBoot;  // defined in main.cpp (RTC_DATA_ATTR)

// Static singleton instance for the SNTP callback to reach
ScheduleManager* ScheduleManager::_instance = nullptr;

ScheduleManager::ScheduleManager() :
    uploadStartHour(8),
    uploadEndHour(22),
    uploadMode("scheduled"),
    uploadHour(12),
    uploadCompletedToday(false),
    lastCompletedDay(-1),
    lastUploadTimestamp(0),
    ntpSynced(false),
    ntpServer("pool.ntp.org"),
    gmtOffsetHours(0)
{}

bool ScheduleManager::begin(const String& mode, int startHour, int endHour,
                            int gmtOffset, const String& tz, const String& ntp) {
    this->uploadMode = mode;
    this->uploadStartHour = startHour;
    this->uploadEndHour = endHour;
    this->gmtOffsetHours = gmtOffset;
    this->tzString = tz;
    this->ntpServer = ntp;
    
    // Also set legacy uploadHour for backward compat
    this->uploadHour = startHour;
    
    if (tzString.length() > 0) {
        LOGF("[Schedule] Mode: %s, Window: %d:00-%d:00, TZ: %s",
             mode.c_str(), startHour, endHour, tzString.c_str());
    } else {
        LOGF("[Schedule] Mode: %s, Window: %d:00-%d:00, GMT%+d",
             mode.c_str(), startHour, endHour, gmtOffset);
    }

    syncTime();
    return true;
}

bool ScheduleManager::begin(int uploadHour, int gmtOffsetHours) {
    // Legacy overload: create a 2-hour window from the single upload hour
    this->uploadHour = uploadHour;
    
    if (uploadHour < 0 || uploadHour > 23) {
        LOG("Invalid upload hour, using default (12)");
        this->uploadHour = 12;
    }
    
    return begin("scheduled", this->uploadHour, (this->uploadHour + 2) % 24, gmtOffsetHours);
}

// callback fired from lwIP task context on every successful ntp sync
void ScheduleManager::ntpSyncCallback(struct timeval *tv) {
    if (!_instance) return;

    bool wasFirstSync = !_instance->ntpSynced;
    _instance->ntpSynced = true;

    if (wasFirstSync) {
        // restore default interval
        sntp_set_sync_interval(CONFIG_LWIP_SNTP_UPDATE_DELAY);
        sntp_restart();

        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 0)) {
            LOGF("[NTP] Time synchronized: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        } else {
            LOG("[NTP] Time synchronized successfully");
        }
        LOGF("[NTP] Re-sync interval restored to %lu ms", (unsigned long)CONFIG_LWIP_SNTP_UPDATE_DELAY);
    }
}

// NTP server selection: config -> DHCP -> pool.ntp.org
void ScheduleManager::configureNtpServers() {
    if (ntpServer.length() > 0) {
        esp_sntp_setservername(0, ntpServer.c_str());
        LOGF("[NTP] Using configured server: %s", ntpServer.c_str());
    } else {
        const char* dhcpNtpServer = esp_sntp_getservername(0);
        if (dhcpNtpServer && dhcpNtpServer[0] != '\0') {
            // DHCP populated slot 0, just keep it.
            LOGF("[NTP] Using DHCP-provided server: %s", dhcpNtpServer);
        } else {
            esp_sntp_setservername(0, "pool.ntp.org");
            LOG("[NTP] Using default server: pool.ntp.org");
        }
    }
}

// syncTime: start SNTP daemon and return immediately
bool ScheduleManager::syncTime() {
    LOG("[NTP] Starting time synchronization...");

    _instance = this;

    // Allow network to stabilize after WiFi connection.
    // Skip on heap-recovery reboots — WiFi re-connects to a known AP in <1 s.
    if (g_heapRecoveryBoot) {
        LOG("[NTP] [FastBoot] Skipping network-stabilize delay");
    } else {
        LOG("[NTP] Waiting 2 seconds for network to stabilize...");
        delay(2000);
    }

    // Set timezone (replicates what configTzTime/configTime do)
    if (tzString.length() > 0) {
        LOGF("[NTP] Using POSIX timezone: %s", tzString.c_str());
        setenv("TZ", tzString.c_str(), 1);
        tzset();
    } else {
        // note inverted sign, TZ "UTC-5" is "GMT+5"
        char tz[24];
        snprintf(tz, sizeof(tz), "UTC%d", -gmtOffsetHours);
        LOGF("[NTP] Using GMT offset: %d hours (TZ=%s)", gmtOffsetHours, tz);
        setenv("TZ", tz, 1);
        tzset();
    }

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    sntp_set_time_sync_notification_cb(ntpSyncCallback);

    // Use aggressive 60-second retry interval until first successful sync,
    sntp_set_sync_interval(60 * 1000);

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    configureNtpServers();

    esp_sntp_init();

    LOG("[NTP] SNTP daemon started");
    return true;
}

// ============================================================================
// Window-based scheduling (new FSM methods)
// ============================================================================

bool ScheduleManager::isInUploadWindow() {
    if (!ntpSynced) return false;
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return false;
    
    // If start == end, window is always open (24/7)
    if (uploadStartHour == uploadEndHour) return true;
    
    int currentHour = timeinfo.tm_hour;
    
    if (uploadStartHour < uploadEndHour) {
        // Normal window: e.g., 8-22
        return currentHour >= uploadStartHour && currentHour < uploadEndHour;
    } else {
        // Cross-midnight window: e.g., 22-6
        return currentHour >= uploadStartHour || currentHour < uploadEndHour;
    }
}

bool ScheduleManager::canUploadFreshData() {
    if (!ntpSynced) return false;
    
    if (isSmartMode()) {
        // Smart mode: fresh data can upload anytime
        return true;
    }
    // Scheduled mode: fresh data only within upload window
    return isInUploadWindow();
}

bool ScheduleManager::canUploadOldData() {
    if (!ntpSynced) return false;
    
    // Both modes: old data only within upload window
    return isInUploadWindow();
}

bool ScheduleManager::isUploadEligible(bool hasFreshData, bool hasOldData) {
    if (!ntpSynced) return false;
    
    // In scheduled mode, check if already completed today
    if (!isSmartMode() && isDayCompleted()) {
        return false;
    }
    
    // Check if any data category is eligible right now
    if (hasFreshData && canUploadFreshData()) return true;
    if (hasOldData && canUploadOldData()) return true;
    
    return false;
}

void ScheduleManager::markDayCompleted() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        lastCompletedDay = timeinfo.tm_yday;
        uploadCompletedToday = true;
    }
    lastUploadTimestamp = time(nullptr);
    LOGF("[Schedule] Day marked as completed (yday=%d)", lastCompletedDay);
}

bool ScheduleManager::isDayCompleted() {
    if (!uploadCompletedToday) return false;
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return false;
    
    // Reset if we're on a new day
    if (timeinfo.tm_yday != lastCompletedDay) {
        uploadCompletedToday = false;
        return false;
    }
    return true;
}

// ============================================================================
// Legacy methods (delegate to new logic)
// ============================================================================

bool ScheduleManager::isUploadTime() {
    if (!ntpSynced) {
        LOG("Time not synced, cannot check upload schedule");
        return false;
    }
    
    // Use new window check + day completion
    if (isDayCompleted()) return false;
    return isInUploadWindow();
}

void ScheduleManager::markUploadCompleted() {
    markDayCompleted();
    LOGF("Upload marked as completed at timestamp: %lu", lastUploadTimestamp);
}

unsigned long ScheduleManager::getSecondsUntilNextUpload() {
    if (!ntpSynced) {
        return 0;
    }
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return 0;
    
    // Calculate seconds until upload window opens
    int currentHour = timeinfo.tm_hour;
    int hoursUntil;
    
    if (uploadStartHour <= uploadEndHour) {
        // Normal window
        if (currentHour < uploadStartHour) {
            hoursUntil = uploadStartHour - currentHour;
        } else if (currentHour >= uploadEndHour) {
            hoursUntil = (24 - currentHour) + uploadStartHour;
        } else {
            return 0; // Currently in window
        }
    } else {
        // Cross-midnight window
        if (currentHour >= uploadStartHour || currentHour < uploadEndHour) {
            return 0; // Currently in window
        }
        hoursUntil = uploadStartHour - currentHour;
        if (hoursUntil < 0) hoursUntil += 24;
    }
    
    // Approximate: hours * 3600 minus current minutes/seconds
    return (unsigned long)hoursUntil * 3600 - timeinfo.tm_min * 60 - timeinfo.tm_sec;
}

// ============================================================================
// Time utilities
// ============================================================================

bool ScheduleManager::isTimeSynced() const {
    return ntpSynced;
}

unsigned long ScheduleManager::getLastUploadTimestamp() const {
    return lastUploadTimestamp;
}

void ScheduleManager::setLastUploadTimestamp(unsigned long timestamp) {
    lastUploadTimestamp = timestamp;
}

String ScheduleManager::getCurrentLocalTime() const {
    if (!ntpSynced) {
        return "Time not synchronized";
    }
    
    time_t now = time(nullptr);
    struct tm timeinfo;
    if (!localtime_r(&now, &timeinfo)) {
        return "Failed to get local time";
    }
    
    char buffer[48];
    if (tzString.length() > 0) {
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d (%s)",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                 timeinfo.tm_isdst > 0 ? "DST" : "STD");
    } else {
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d (GMT%+d)",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                 gmtOffsetHours);
    }

    
    return String(buffer);
}

// ============================================================================
// Getters for web UI
// ============================================================================

const String& ScheduleManager::getUploadMode() const { return uploadMode; }
int ScheduleManager::getUploadStartHour() const { return uploadStartHour; }
int ScheduleManager::getUploadEndHour() const { return uploadEndHour; }
bool ScheduleManager::isSmartMode() const { return uploadMode == "smart"; }
