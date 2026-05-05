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

    // beginConnect/pollConnect state. _pendingSsid/Password retained so PMF
    // retry can re-issue the connection without the caller passing them again.
    ConnectPhase _connectPhase;
    uint32_t     _phaseStartMs;
    String       _pendingSsid;
    String       _pendingPassword;

    static constexpr uint32_t CONNECT_PHASE_TIMEOUT_MS = 15000;

    static volatile uint8_t _lastDisconnectReason;  // Captured in event handler for retry logic
    static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

    void enterPhase(ConnectPhase newPhase);
    void enterPmfRetry();
    void terminateConnect(ConnectPhase result);  // CONNECTED or FAILED
    void logConnectFailure();

public:
    WiFiManager();

    void setupEventHandlers();

    // Synchronous connect (boot path)
    bool connectStation(const String& ssid, const String& password, const String& hostname);

    // Non-blocking connect API (loop reconnect path).
    // beginConnect() starts an attempt; returns false on invalid args or if an
    // attempt is already in progress. pollConnect() must be called from the
    // main loop until isConnectInProgress() returns false.
    bool beginConnect(const String& ssid, const String& password, const String& hostname);
    void pollConnect();
    bool isConnectInProgress() const;
    ConnectPhase getConnectPhase() const { return _connectPhase; }
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
