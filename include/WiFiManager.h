#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <DNSServer.h>
#include "Config.h"

// Forward declarations for power management enums
enum class WifiTxPower;
enum class WifiPowerSaving;

class WiFiManager {
public:
    // Connection state machine
    enum class ConnectPhase {
        IDLE,       // no attempt in progress
        SCANNING,   // async WiFi.scanNetworks() in progress (multi-SSID only)
        CONNECTING, // WiFi.begin() called with PMF enabled (default), awaiting result
        PMF_RETRY,  // PMF disabled after reason-208 disconnect, awaiting result
        CONNECTED,  // success, terminal
        FAILED      // failure, terminal (timeout, no AP, auth fail...)
    };

private:
    bool connected;
    bool mdnsStarted;
    bool apMode;
    DNSServer dnsServer;
    int8_t _pendingTxPower;   // Deferred TX power (dBm*4), applied during beginConnect()
    bool _hasPendingTxPower;

    // One scan-result entry that matched a configured SSID.  Built during the
    // SCANNING phase, sorted by RSSI desc, then tried in order.
    struct Candidate {
        uint8_t configSlot;   // index into Config.wifiNetworks[]
        uint8_t bssid[6];     // BSSID from scan result (used as connect hint)
        uint8_t channel;      // 1..14 (used as connect hint)
        int8_t  rssi;         // dBm
    };

    static constexpr int     MAX_CANDIDATES        = 12;
    static constexpr uint8_t CANDIDATE_MAX_RETRIES = 2;
    static constexpr uint32_t SCAN_TIMEOUT_MS      = 12000;

    // beginConnect/pollConnect state. _pendingSsid/Password retained so PMF
    // retry can re-issue the connection without the caller passing them again.
    ConnectPhase _connectPhase;
    uint32_t     _phaseStartMs;
    String       _pendingSsid;
    String       _pendingPassword;
    uint8_t      _consecutiveFailures;

    // Multi-SSID candidate iteration.
    Candidate    _candidates[MAX_CANDIDATES];
    uint8_t      _candidateCount;
    uint8_t      _candidateIndex;
    uint8_t      _candidateRetries;
    const Config* _pendingConfig;
    String       _pendingHostname;

    static constexpr uint32_t CONNECT_PHASE_TIMEOUT_MS = 15000;

    static volatile uint8_t _lastDisconnectReason;  // Captured in event handler for retry logic
    static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

    void enterPhase(ConnectPhase newPhase);
    void enterPmfRetry();
    void terminateConnect(ConnectPhase result);  // CONNECTED or FAILED
    void logConnectFailure();

    void prepareSingleCandidate(uint8_t configSlot);
    void kickScan();
    void processScanResults();
    bool startCurrentCandidate();
    void onCurrentCandidateFailed();

public:
    WiFiManager();

    void setupEventHandlers();

    // Multi-SSID connect. Reads up to WIFI_MAX_NETWORKS slots from Config.
    // With 1 populated slot: direct WiFi.begin (no scan).
    // With 2+: async scan first, ranks visible APs by RSSI, tries each
    // connectStation() is the synchronous wrapper used at boot
    bool connectStation(const Config& cfg);
    bool beginConnect(const Config& cfg);

    void pollConnect();
    bool isConnectInProgress() const;
    ConnectPhase getConnectPhase() const { return _connectPhase; }

    // Recommended interval before the next reconnect attempt, in ms.
    // Schedule (consecutive FAILED count -> interval): 0->30s, 1->60s,
    // 2->2min, 3+->5min. Reset to 30s after a CONNECTED.
    uint32_t getReconnectIntervalMs() const;
    uint8_t  getConsecutiveFailures() const { return _consecutiveFailures; }
    void startAP();
    void processDNS();
    bool isAPMode() const { return apMode; }
    
    bool isConnected() const;
    void disconnect();
    String getIPAddress() const;
    int getSignalStrength() const;    // Returns RSSI in dBm
    String getSignalQuality() const;  // Returns quality description
    
    // mDNS support
    bool startMDNS(const String& hostname);
    
    // Power management methods
    void setHighPerformanceMode();    // Disable power save for uploads
    void setPowerSaveMode();          // Enable power save for idle periods
    void setMaxPowerSave();           // Maximum power savings
    void applyTxPowerEarly(WifiTxPower txPower);  // Set TX power before WiFi.begin()
    void applyPowerSettings(WifiTxPower txPower, WifiPowerSaving powerSaving);  // Apply config settings
};

#endif // WIFI_MANAGER_H
