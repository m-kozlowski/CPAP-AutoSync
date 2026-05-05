#include "WiFiManager.h"
#include "Logger.h"
#include "Config.h"  // For power management enums
#include "SDCardManager.h"
#include "NetworkHints.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <time.h>

extern NetworkHints networkHints; // defined in main.cpp

volatile uint8_t WiFiManager::_lastDisconnectReason = 0;

WiFiManager::WiFiManager()
    : connected(false), mdnsStarted(false), apMode(false),
      _pendingTxPower(0), _hasPendingTxPower(false),
      _connectPhase(ConnectPhase::IDLE), _phaseStartMs(0),
      _consecutiveFailures(0),
      _candidateCount(0), _candidateIndex(0), _candidateRetries(0),
      _pendingConfig(nullptr),
      _pendingPmfDisable(false) {}

void WiFiManager::startAP() {
    LOG("Starting AP Mode (CPAP-AutoSync) for configuration");
    // Cleanly tear down any lingering STA connection before switching to AP.
    // Without this, the radio may still be in STA mode after a failed connect,
    // causing softAP to silently fail or not broadcast.
    WiFi.disconnect(true);  // disconnect + erase STA credentials from RAM
    WiFi.mode(WIFI_OFF);
    delay(100);  // let radio settle
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAP("CPAP-AutoSync");
    
    LOGF("AP IP address: %s", WiFi.softAPIP().toString().c_str());
    
    // Start Captive Portal DNS server
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());
    apMode = true;
}

void WiFiManager::processDNS() {
    if (apMode) {
        dnsServer.processNextRequest();
    }
}

void WiFiManager::setupEventHandlers() {
    WiFi.onEvent(onWiFiEvent);
    LOG_DEBUG("WiFi event handlers registered");
}

void WiFiManager::onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_READY:
            LOG_DEBUG("WiFi Event: WiFi interface ready");
            break;
            
        case ARDUINO_EVENT_WIFI_SCAN_DONE:
            LOG_DEBUG("WiFi Event: Scan completed");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_START:
            LOG_INFO("WiFi Event: Station mode started");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_STOP:
            LOG_INFO("WiFi Event: Station mode stopped");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            LOG_INFO("WiFi Event: Connected to AP");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            uint8_t reason = info.wifi_sta_disconnected.reason;
            _lastDisconnectReason = reason;
            LOG_WARNF("WiFi Event: Disconnected from AP (reason: %d)", reason);
            
            // Log human-readable disconnect reasons
            switch (reason) {
                case WIFI_REASON_UNSPECIFIED:
                    LOG_WARN("Disconnect reason: Unspecified");
                    break;
                case WIFI_REASON_AUTH_EXPIRE:
                    LOG_WARN("Disconnect reason: Authentication expired");
                    break;
                case WIFI_REASON_AUTH_LEAVE:
                    LOG_WARN("Disconnect reason: Deauthenticated (left network)");
                    break;
                case WIFI_REASON_ASSOC_EXPIRE:
                    LOG_WARN("Disconnect reason: Association expired");
                    break;
                case WIFI_REASON_ASSOC_TOOMANY:
                    LOG_WARN("Disconnect reason: Too many associations");
                    break;
                case WIFI_REASON_NOT_AUTHED:
                    LOG_WARN("Disconnect reason: Not authenticated");
                    break;
                case WIFI_REASON_NOT_ASSOCED:
                    LOG_WARN("Disconnect reason: Not associated");
                    break;
                case WIFI_REASON_ASSOC_LEAVE:
                    LOG_WARN("Disconnect reason: Disassociated (left network)");
                    break;
                case WIFI_REASON_ASSOC_NOT_AUTHED:
                    LOG_WARN("Disconnect reason: Association not authenticated");
                    break;
                case WIFI_REASON_DISASSOC_PWRCAP_BAD:
                    LOG_WARN("Disconnect reason: Bad power capability");
                    break;
                case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
                    LOG_WARN("Disconnect reason: Bad supported channels");
                    break;
                case WIFI_REASON_IE_INVALID:
                    LOG_WARN("Disconnect reason: Invalid information element");
                    break;
                case WIFI_REASON_MIC_FAILURE:
                    LOG_WARN("Disconnect reason: MIC failure");
                    break;
                case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                    LOG_WARN("Disconnect reason: 4-way handshake timeout");
                    break;
                case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
                    LOG_WARN("Disconnect reason: Group key update timeout");
                    break;
                case WIFI_REASON_IE_IN_4WAY_DIFFERS:
                    LOG_WARN("Disconnect reason: IE in 4-way handshake differs");
                    break;
                case WIFI_REASON_GROUP_CIPHER_INVALID:
                    LOG_WARN("Disconnect reason: Invalid group cipher");
                    break;
                case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
                    LOG_WARN("Disconnect reason: Invalid pairwise cipher");
                    break;
                case WIFI_REASON_AKMP_INVALID:
                    LOG_WARN("Disconnect reason: Invalid AKMP");
                    break;
                case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
                    LOG_WARN("Disconnect reason: Unsupported RSN IE version");
                    break;
                case WIFI_REASON_INVALID_RSN_IE_CAP:
                    LOG_WARN("Disconnect reason: Invalid RSN IE capability");
                    break;
                case WIFI_REASON_802_1X_AUTH_FAILED:
                    LOG_WARN("Disconnect reason: 802.1X authentication failed");
                    break;
                case WIFI_REASON_CIPHER_SUITE_REJECTED:
                    LOG_WARN("Disconnect reason: Cipher suite rejected");
                    break;
                case WIFI_REASON_BEACON_TIMEOUT:
                    LOG_WARN("Disconnect reason: Beacon timeout (AP lost)");
                    break;
                case WIFI_REASON_NO_AP_FOUND:
                    LOG_WARN("Disconnect reason: No AP found");
                    break;
                case WIFI_REASON_AUTH_FAIL:
                    LOG_WARN("Disconnect reason: Authentication failed");
                    break;
                case WIFI_REASON_ASSOC_FAIL:
                    LOG_WARN("Disconnect reason: Association failed");
                    break;
                case WIFI_REASON_HANDSHAKE_TIMEOUT:
                    LOG_WARN("Disconnect reason: Handshake timeout");
                    break;
                case WIFI_REASON_CONNECTION_FAIL:
                    LOG_WARN("Disconnect reason: Connection failed");
                    break;
                case WIFI_REASON_AP_TSF_RESET:
                    LOG_WARN("Disconnect reason: AP TSF reset");
                    break;
                case WIFI_REASON_ROAMING:
                    LOG_WARN("Disconnect reason: Roaming");
                    break;
                case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG:
                    LOG_WARN("Disconnect reason: Association comeback time too long (PMF/802.11w)");
                    break;
                case WIFI_REASON_SA_QUERY_TIMEOUT:
                    LOG_WARN("Disconnect reason: SA query timeout (PMF/802.11w)");
                    break;
                default:
                    LOG_WARNF("Disconnect reason: Unknown (%d)", reason);
                    break;
            }
            break;
        }
            
        case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
            LOG_DEBUG("WiFi Event: Authentication mode changed");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            LOG_INFOF("WiFi Event: Got IP address: %s", WiFi.localIP().toString().c_str());
            break;
            
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            LOG_WARN("WiFi Event: Lost IP address");
            break;
            
        default:
            LOG_DEBUGF("WiFi Event: Unhandled event %d", event);
            break;
    }
}

// Connection state machine
//
// beginConnect() validates args, sets WIFI mode (STA, or AP_STA if a softAP
// is already running), kicks off WiFi.begin(), and transitions to CONNECTING.
// pollConnect() advances the state machine on each call from the main loop:
// reads WiFi.status() and the last disconnect reason, handles the PMF retry
// sub-state, and transitions to CONNECTED or FAILED on terminal results.
// connectStation() is a synchronous wrapper used by the boot path.

void WiFiManager::enterPhase(ConnectPhase newPhase) {
    _connectPhase = newPhase;
    _phaseStartMs = millis();
}

bool WiFiManager::isConnectInProgress() const {
    return _connectPhase == ConnectPhase::SCANNING  ||
           _connectPhase == ConnectPhase::CONNECTING ||
           _connectPhase == ConnectPhase::PMF_RETRY;
}

void WiFiManager::terminateConnect(ConnectPhase result) {
    _connectPhase = result;
    _phaseStartMs = millis();
    if (result == ConnectPhase::CONNECTED) {
        connected = true;
        _consecutiveFailures = 0;
        LOG("WiFi connected");
        LOGF("IP address: %s", WiFi.localIP().toString().c_str());

        // Persist a hint for the AP we just connected to. WiFi.BSSID() and
        // WiFi.channel() are authoritative now (the candidate's values may
        // have been zero on the single-SSID direct path).
        if (_pendingConfig && _candidateIndex < _candidateCount) {
            uint8_t configSlot = _candidates[_candidateIndex].configSlot;
            const String& ssid = _pendingConfig->getWifiSSID(configSlot);
            const uint8_t* bssid = WiFi.BSSID();
            uint8_t channel = (uint8_t)WiFi.channel();
            if (bssid && channel > 0 && !ssid.isEmpty()) {
                if (networkHints.upsert(ssid.c_str(), bssid, channel, _pendingPmfDisable)) {
                    networkHints.save();
                    LOGF("WiFi: hint saved for '%s' ch=%u%s",
                         ssid.c_str(), channel,
                         _pendingPmfDisable ? " (PMF disabled)" : "");
                }
            }
        }
    } else {
        connected = false;
        if (_consecutiveFailures < 0xFF) _consecutiveFailures++;
        logConnectFailure();
        LOGF("WiFi reconnect backoff: next attempt in %lu ms (failure #%u)",
             (unsigned long)getReconnectIntervalMs(), (unsigned)_consecutiveFailures);
        Logger::getInstance().dumpSavedLogs("wifi_connection_failed");
    }
}

uint32_t WiFiManager::getReconnectIntervalMs() const {
    switch (_consecutiveFailures) {
        case 0:  return  30 * 1000UL;
        case 1:  return  60 * 1000UL;
        case 2:  return 120 * 1000UL;
        default: return 300 * 1000UL;  // capped at 5 min
    }
}

void WiFiManager::logConnectFailure() {
    LOGF("WiFi connection failed, status: %d", WiFi.status());
    switch (WiFi.status()) {
        case WL_NO_SSID_AVAIL:  LOG_ERROR("WiFi failure: SSID not found"); break;
        case WL_CONNECT_FAILED: LOG_ERROR("WiFi failure: Connection failed (wrong password?)"); break;
        case WL_CONNECTION_LOST:LOG_ERROR("WiFi failure: Connection lost"); break;
        case WL_DISCONNECTED:   LOG_ERROR("WiFi failure: Disconnected"); break;
        default:                LOGF("WiFi failure: Unknown status %d", WiFi.status()); break;
    }
}

void WiFiManager::enterPmfRetry() {
    LOG_WARN("PMF association comeback timeout (reason 208) - retrying with PMF disabled");
    wifi_config_t wifi_conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_conf) != ESP_OK) {
        onCurrentCandidateFailed();
        return;
    }
    wifi_conf.sta.pmf_cfg.capable  = false;
    wifi_conf.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &wifi_conf);
    esp_wifi_disconnect();
    esp_wifi_connect();
    _pendingPmfDisable = true;
    enterPhase(ConnectPhase::PMF_RETRY);
}

// Multi-SSID candidate pipeline
//
// beginConnect(Config&) prepares common radio state once, then either
// (1 network) builds a single candidate with no BSSID hint and connects, or
// (2+ networks) kicks an async scan; pollConnect() processes scan completion,
// builds candidate list (visible APs intersected with configured SSIDs,
// sorted by RSSI), and tries each candidate in turn with up to
// CANDIDATE_MAX_RETRIES retries before falling back to the next.

void WiFiManager::prepareSingleCandidate(uint8_t configSlot) {
    _candidates[0].configSlot = configSlot;
    memset(_candidates[0].bssid, 0, 6);
    _candidates[0].channel = 0;
    _candidates[0].rssi    = 0;

   // Single-SSID path: no scan to source a BSSID/channel from. Try to find
   // a hint for this SSID (any BSSID; pick the most recently used) so we can
   // skip the radio's own scan during association.
   if (_pendingConfig) {
       const String& ssid = _pendingConfig->getWifiSSID(configSlot);
       const SavedNetworkHint* best = nullptr;
       for (int i = 0; i < networkHints.count(); i++) {
           const SavedNetworkHint* h = networkHints.at(i);
           if (h && strcmp(h->ssid, ssid.c_str()) == 0) {
               if (!best || h->last_used_secs > best->last_used_secs) best = h;
           }
       }
       if (best) {
           memcpy(_candidates[0].bssid, best->bssid, 6);
           _candidates[0].channel = best->channel;
           LOGF("WiFi: using cached hint for '%s' (ch=%u)", ssid.c_str(), best->channel);
       }
   }

    _candidateCount   = 1;
    _candidateIndex   = 0;
    _candidateRetries = 0;
}

void WiFiManager::kickScan() {
    LOGF("WiFi: scanning for %d configured network(s)...", _pendingConfig->getWifiNetworkCount());
    int rc = WiFi.scanNetworks(true /*async*/, false /*show_hidden*/);
    if (rc < 0 && rc != WIFI_SCAN_RUNNING) {
        LOG_ERRORF("WiFi scan kick failed: %d", rc);
        terminateConnect(ConnectPhase::FAILED);
        return;
    }
    enterPhase(ConnectPhase::SCANNING);
}

void WiFiManager::processScanResults() {
    int n = WiFi.scanComplete();
    if (n < 0) {
        LOG_WARNF("processScanResults() called with rc=%d", n);
        WiFi.scanDelete();
        terminateConnect(ConnectPhase::FAILED);
        return;
    }

    int cfgCount = _pendingConfig->getWifiNetworkCount();
    _candidateCount = 0;

    for (int i = 0; i < n && _candidateCount < MAX_CANDIDATES; i++) {
        String visibleSsid = WiFi.SSID(i);
        for (int slot = 0; slot < cfgCount; slot++) {
            if (visibleSsid != _pendingConfig->getWifiSSID(slot)) continue;
            Candidate& c = _candidates[_candidateCount];
            c.configSlot = (uint8_t)slot;
            const uint8_t* bssid = WiFi.BSSID(i);
            if (bssid) memcpy(c.bssid, bssid, 6);
            else       memset(c.bssid, 0, 6);
            c.channel = (uint8_t)WiFi.channel(i);
            c.rssi    = (int8_t)WiFi.RSSI(i);
            _candidateCount++;
            break;  // one scan result matches at most one configured slot
        }
    }

    WiFi.scanDelete();
    LOGF("WiFi scan: %d known of %d visible", _candidateCount, n);

    if (_candidateCount == 0) {
        terminateConnect(ConnectPhase::FAILED);
        return;
    }

    // Sort candidates by RSSI desc (insertion sort - tiny array)
    for (int i = 1; i < _candidateCount; i++) {
        Candidate tmp = _candidates[i];
        int j = i - 1;
        while (j >= 0 && _candidates[j].rssi < tmp.rssi) {
            _candidates[j + 1] = _candidates[j];
            j--;
        }
        _candidates[j + 1] = tmp;
    }

    LOGF("WiFi: best candidate '%s' %d dBm",
         _pendingConfig->getWifiSSID(_candidates[0].configSlot).c_str(),
         _candidates[0].rssi);

    _candidateIndex   = 0;
    _candidateRetries = 0;
    startCurrentCandidate();
}

bool WiFiManager::startCurrentCandidate() {
    if (_candidateIndex >= _candidateCount || !_pendingConfig) {
        terminateConnect(ConnectPhase::FAILED);
        return false;
    }
    Candidate& c = _candidates[_candidateIndex];
    _pendingSsid     = _pendingConfig->getWifiSSID(c.configSlot);
    _pendingPassword = _pendingConfig->getWifiPassword(c.configSlot);
    _lastDisconnectReason = 0;

    // Look up persisted hint for this exact (ssid, bssid). Used to pre-set the
    // PMF-disable flag on routers we already know reject 802.11w
    _pendingPmfDisable = false;
    bool hasBssid = (c.bssid[0] | c.bssid[1] | c.bssid[2] |
                     c.bssid[3] | c.bssid[4] | c.bssid[5]) != 0;
    if (hasBssid) {
        const SavedNetworkHint* hint = networkHints.find(_pendingSsid.c_str(), c.bssid);
        if (hint && hint->pmf_disable) {
            _pendingPmfDisable = true;
            LOGF("WiFi: hint says PMF disabled for '%s'", _pendingSsid.c_str());
        }
    }

    bool hasHint = (c.channel > 0);
    if (hasHint) {
        LOGF("WiFi: connecting to '%s' (ch=%u, RSSI %d dBm)",
             _pendingSsid.c_str(), c.channel, c.rssi);
        WiFi.begin(_pendingSsid.c_str(), _pendingPassword.c_str(), c.channel, c.bssid);
    } else {
        LOGF("WiFi: connecting to '%s'", _pendingSsid.c_str());
        WiFi.begin(_pendingSsid.c_str(), _pendingPassword.c_str());
    }

    // If we already know this AP needs PMF disabled, override the default
    // (capable=true) configuration immediately. WiFi.begin() set the SSID/pass
    // via esp_wifi_set_config; we override pmf_cfg and reconnect, course-
    // correcting the in-flight association before it has a chance to fail
    // with reason 208.
    if (_pendingPmfDisable) {
        wifi_config_t wifi_conf;
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_conf) == ESP_OK) {
            wifi_conf.sta.pmf_cfg.capable  = false;
            wifi_conf.sta.pmf_cfg.required = false;
            esp_wifi_set_config(WIFI_IF_STA, &wifi_conf);
            esp_wifi_disconnect();
            esp_wifi_connect();
        }
    }

    // Apply deferred TX power AFTER WiFi.begin() — WiFi.mode(WIFI_STA) is async
    // and the STA isn't fully started until the STA_START event fires. begin()
    // blocks until STA is active, so setTxPower() works reliably here.
    // This caps TX power during the association/DHCP phase.
    if (_hasPendingTxPower) {
        WiFi.setTxPower((wifi_power_t)_pendingTxPower);
        LOG_DEBUGF("WiFi TX power applied: %d (deferred from applyTxPowerEarly)", (int)_pendingTxPower);
        _hasPendingTxPower = false;
    }

    enterPhase(ConnectPhase::CONNECTING);
    return true;
}

void WiFiManager::onCurrentCandidateFailed() {
    if (_candidateRetries < CANDIDATE_MAX_RETRIES) {
        _candidateRetries++;
        LOGF("WiFi: retry %u/%u on current candidate", _candidateRetries, CANDIDATE_MAX_RETRIES);
        startCurrentCandidate();
        return;
    }
    _candidateIndex++;
    _candidateRetries = 0;
    if (_candidateIndex < _candidateCount) {
        LOGF("WiFi: trying next candidate (%u/%u)", _candidateIndex + 1, _candidateCount);
        startCurrentCandidate();
        return;
    }
    terminateConnect(ConnectPhase::FAILED);
}

bool WiFiManager::beginConnect(const Config& cfg) {
    if (isConnectInProgress()) {
        LOG_WARN("beginConnect() called while attempt already in progress");
        return false;
    }
    int n = cfg.getWifiNetworkCount();
    if (n <= 0) {
        LOG_ERROR("Cannot connect to WiFi: no networks configured");
        Logger::getInstance().dumpSavedLogs("wifi_config_error");
        return false;
    }

    LOGF("WiFi: %d configured network(s)", n);

    const String& hostname = cfg.getHostname();
    if (!hostname.isEmpty()) {
        WiFi.setHostname(hostname.c_str());
        LOGF("DHCP Hostname set to: %s", hostname.c_str());
    }

    // AP+STA coexistence: if a softAP is already running, switch to AP_STA mode
    // so the AP keeps broadcasting while STA scans/connects. Otherwise plain STA.
    WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);

    // ── Power optimization: disable 802.11b (DSSS) ──
    // 802.11b uses up to 370 mA peak TX. Restricting to 802.11g/n (OFDM)
    // caps peak TX current to ~205-250 mA. All modern routers support g/n.
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    LOG_DEBUG("WiFi protocol restricted to 802.11g/n (802.11b disabled)");

    // Enable DHCP NTP server capture before connecting.
    // This passively stores any NTP servers the router advertises;
    // ScheduleManager decides whether to use them based on config priority.
    esp_sntp_servermode_dhcp(true);

    _pendingConfig    = &cfg;
    _pendingHostname  = cfg.getHostname();
    _candidateCount   = 0;
    _candidateIndex   = 0;
    _candidateRetries = 0;

    if (n == 1) {
        // Single SSID: skip scan, no BSSID hint.
        prepareSingleCandidate(0);
        return startCurrentCandidate();
    }
    // Multi-SSID: scan first.
    kickScan();
    return true;
}

void WiFiManager::pollConnect() {
    if (!isConnectInProgress()) return;

    if (_connectPhase == ConnectPhase::SCANNING) {
        int rc = WiFi.scanComplete();
        if (rc >= 0) {
            processScanResults();
            return;
        }
        if (rc == WIFI_SCAN_FAILED) {
            LOG_ERROR("WiFi scan failed");
            WiFi.scanDelete();
            terminateConnect(ConnectPhase::FAILED);
            return;
        }
        if (millis() - _phaseStartMs >= SCAN_TIMEOUT_MS) {
            LOG_WARN("WiFi scan timeout");
            WiFi.scanDelete();
            terminateConnect(ConnectPhase::FAILED);
            return;
        }
        return;  // scan still running
    }

    // CONNECTING or PMF_RETRY
    wl_status_t status = WiFi.status();
    uint32_t elapsed = millis() - _phaseStartMs;

    if (status == WL_CONNECTED) {
        terminateConnect(ConnectPhase::CONNECTED);
        return;
    }
    if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED) {
        onCurrentCandidateFailed();
        return;
    }
    if (elapsed >= CONNECT_PHASE_TIMEOUT_MS) {
        if (_connectPhase == ConnectPhase::CONNECTING &&
            _lastDisconnectReason == WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG) {
            enterPmfRetry();
            return;
        }
        onCurrentCandidateFailed();
        return;
    }
    // Still in progress - nothing to do this tick.
}

bool WiFiManager::connectStation(const Config& cfg) {
    if (!beginConnect(cfg)) return false;
    while (isConnectInProgress()) {
        pollConnect();
        if (isConnectInProgress()) delay(50);
    }
    return _connectPhase == ConnectPhase::CONNECTED;
}

bool WiFiManager::isConnected() const { 
    return connected && WiFi.status() == WL_CONNECTED; 
}

void WiFiManager::disconnect() {
    if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
    }
    WiFi.disconnect();
    connected = false;
}

String WiFiManager::getIPAddress() const {
    if (connected && WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "Not connected";
}

int WiFiManager::getSignalStrength() const {
    if (connected && WiFi.status() == WL_CONNECTED) {
        return WiFi.RSSI();  // Returns signal strength in dBm
    }
    return 0;
}

String WiFiManager::getSignalQuality() const {
    if (!connected || WiFi.status() != WL_CONNECTED) {
        return "Not connected";
    }
    
    int rssi = WiFi.RSSI();
    
    // Classify signal strength based on RSSI value
    // RSSI ranges: Excellent > -50, Good > -60, Fair > -70, Weak > -80, Very Weak <= -80
    if (rssi > -50) {
        return "Excellent";
    } else if (rssi > -60) {
        return "Good";
    } else if (rssi > -70) {
        return "Fair";
    } else if (rssi > -80) {
        return "Weak";
    } else {
        return "Very Weak";
    }
}

bool WiFiManager::startMDNS(const String& hostname) {
    if (!connected || WiFi.status() != WL_CONNECTED) {
        LOG_WARN("Cannot start mDNS: WiFi not connected");
        return false;
    }

    LOGF("Starting mDNS responder with hostname: %s.local", hostname.c_str());

    // Ensure stale responder state from prior reconnects is released first.
    if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
        delay(10);
    }
    
    if (MDNS.begin(hostname.c_str())) {
        LOG("mDNS responder started successfully");
        // Advertise web server service
        MDNS.addService("http", "tcp", 80);
        mdnsStarted = true;
        return true;
    } else {
        LOG_ERROR("Failed to start mDNS responder");
        mdnsStarted = false;
        return false;
    }
}

// Power management methods
void WiFiManager::setHighPerformanceMode() {
    if (connected && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);  // Disable all power saving
        LOG_DEBUG("WiFi set to high performance mode (no power saving)");
    }
}

void WiFiManager::setPowerSaveMode() {
    if (connected && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(WIFI_PS_MIN_MODEM);  // Enable minimum modem sleep
        LOG_DEBUG("WiFi set to power save mode (WIFI_PS_MIN_MODEM)");
    }
}

void WiFiManager::setMaxPowerSave() {
    if (connected && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(WIFI_PS_MAX_MODEM);  // Enable maximum modem sleep
        LOG_DEBUG("WiFi set to maximum power save mode (WIFI_PS_MAX_MODEM)");
    }
}
void WiFiManager::applyTxPowerEarly(WifiTxPower txPower) {
    // Store the desired TX power for deferred application inside connectStation().
    // We cannot call WiFi.setTxPower() here because WiFi.mode(WIFI_STA) hasn't
    // been called yet (or hasn't fully started), which causes the warning:
    //   "Neither AP or STA has been started"
    // connectStation() applies it right after WiFi.mode(WIFI_STA).
    wifi_power_t espTxPower;
    switch (txPower) {
        case WifiTxPower::POWER_MAX:    espTxPower = WIFI_POWER_11dBm;       break; // PHY caps at 10 dBm
        case WifiTxPower::POWER_HIGH:   espTxPower = WIFI_POWER_8_5dBm;      break;
        case WifiTxPower::POWER_MID:    espTxPower = WIFI_POWER_5dBm;        break;
        case WifiTxPower::POWER_LOW:    espTxPower = WIFI_POWER_2dBm;        break;
        case WifiTxPower::POWER_LOWEST: espTxPower = WIFI_POWER_MINUS_1dBm;  break;
        default:                        espTxPower = WIFI_POWER_5dBm;        break;
    }
    _pendingTxPower = (int8_t)espTxPower;
    _hasPendingTxPower = true;
    LOG_DEBUGF("WiFi TX power deferred: %d (will apply in connectStation)", (int)espTxPower);
}

void WiFiManager::applyPowerSettings(WifiTxPower txPower, WifiPowerSaving powerSaving) {
    if (!connected || WiFi.status() != WL_CONNECTED) {
        LOG_WARN("Cannot apply power settings - WiFi not connected");
        return;
    }
    
    // Apply TX power setting
    wifi_power_t espTxPower;
    switch (txPower) {
        case WifiTxPower::POWER_MAX:
            espTxPower = WIFI_POWER_11dBm;       // PHY caps at 10 dBm
            LOG_DEBUG("WiFi TX power set to MAX (10dBm, PHY-capped)");
            break;
        case WifiTxPower::POWER_HIGH:
            espTxPower = WIFI_POWER_8_5dBm;      // 8.5dBm
            LOG_DEBUG("WiFi TX power set to HIGH (8.5dBm)");
            break;
        case WifiTxPower::POWER_MID:
            espTxPower = WIFI_POWER_5dBm;        // 5dBm (default)
            LOG_DEBUG("WiFi TX power set to MID (5dBm)");
            break;
        case WifiTxPower::POWER_LOW:
            espTxPower = WIFI_POWER_2dBm;        // 2dBm
            LOG_DEBUG("WiFi TX power set to LOW (2dBm)");
            break;
        case WifiTxPower::POWER_LOWEST:
            espTxPower = WIFI_POWER_MINUS_1dBm;  // -1dBm
            LOG_DEBUG("WiFi TX power set to LOWEST (-1dBm)");
            break;
        default:
            espTxPower = WIFI_POWER_5dBm;
            LOG_WARN("Unknown TX power setting, using MID (5dBm)");
            break;
    }
    WiFi.setTxPower(espTxPower);
    
    // Apply power saving setting
    switch (powerSaving) {
        case WifiPowerSaving::SAVE_NONE:
            WiFi.setSleep(false);
            LOG_DEBUG("WiFi power saving disabled (high performance)");
            break;
        case WifiPowerSaving::SAVE_MID:
            WiFi.setSleep(WIFI_PS_MIN_MODEM);
            LOG_DEBUG("WiFi power saving set to MID (WIFI_PS_MIN_MODEM)");
            break;
        case WifiPowerSaving::SAVE_MAX:
            WiFi.setSleep(WIFI_PS_MAX_MODEM);
            // Set listen_interval so the radio sleeps for multiple DTIM intervals.
            // Without this, MAX_MODEM wakes on every DTIM (~100-300ms), providing
            // minimal benefit over MIN_MODEM. A value of 10 means wake every 10th
            // DTIM beacon, significantly reducing idle radio-on time.
            {
                wifi_config_t wifi_cfg;
                if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
                    wifi_cfg.sta.listen_interval = 10;
                    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
                    LOG_DEBUG("WiFi power saving set to MAX (WIFI_PS_MAX_MODEM, listen_interval=10)");
                } else {
                    LOG_DEBUG("WiFi power saving set to MAX (WIFI_PS_MAX_MODEM, listen_interval unchanged)");
                }
            }
            break;
        default:
            WiFi.setSleep(false);
            LOG_WARN("Unknown power saving setting, disabling power save");
            break;
    }
}
