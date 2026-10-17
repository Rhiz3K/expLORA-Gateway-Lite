/**
 * expLORA Gateway
 *
 * Uplink - connectivity policy header file
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2025
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <functional>
#include "../config.h"
#include "../Data/Logging.h"
#include "../Storage/ConfigManager.h"
#if defined(HAS_ETHERNET)
#include <ETH.h>
#endif

/**
 * Connectivity policy
 *
 * Decides which interfaces run and which one carries traffic; the interfaces
 * themselves are the core's NetworkInterface objects (WiFi.STA, WiFi.AP, ...),
 * so clients (MQTT, HTTP, NTP) just use NetworkClient and the default route.
 *
 * - ethernet (boards with HAS_ETHERNET): DHCP, preferred over WiFi through
 *   the netif route priority, so the default route follows link state
 * - station: connects with the configured credentials, retries every
 *   WIFI_RECONNECT_INTERVAL while down
 * - access point: up for AP_TIMEOUT after local access (ethernet or station)
 *   comes up, and whenever local access is lost (immediately when there are no
 *   WiFi credentials, otherwise after AP_RESTORE_DELAY)
 * - captive portal (DNS + config UI) whenever the AP is up without local access
 * - time: SNTP is (re)started when the device goes online, without blocking
 */
class Uplink
{
public:
    enum class Type : uint8_t
    {
        None,
        Ethernet,
        WiFi,
    };

    Uplink(ConfigManager &config, Logger &log);

    // Start AP and/or station; waits up to WIFI_BOOT_CONNECT_TIMEOUT for the station
    void begin();

    // Call from loop()
    void process();

    // Some uplink has an IP address
    bool isOnline() const { return active != Type::None; }

    // The gateway is reachable on a local network (ethernet or station has an IP)
    bool hasLocalAccess() const;

    // Interface carrying traffic and its address
    Type getActive() const { return active; }
    const char *getActiveName() const;
    IPAddress getLocalIP() const;

    // Access point
    bool isApActive() const { return apActive; }
    bool isCaptive() const { return captive; }
    const String &getApName() const { return apName; }
    IPAddress getApIP() const;

    // Ethernet state (false on boards without ethernet)
    bool ethernetLinkUp() const;
    IPAddress getEthernetIP() const;


    // Called when the captive portal turns on or off
    void setCaptiveCallback(std::function<void(bool)> callback) { captiveCallback = callback; }

    // Called when the interface carrying traffic changes (open connections keep
    // the old source address, so clients should reconnect)
    void setActiveChangeCallback(std::function<void(Type)> callback) { activeChangeCallback = callback; }

private:
    ConfigManager &config;
    Logger &logger;

    String apName;
    bool apActive = false;
    unsigned long lastApAttempt = 0;
    bool captive = false;
    Type active = Type::None;

    bool localWasUp = false;
    unsigned long localAccessSince = 0; // when local access came up
    unsigned long localAccessLostAt = 0;
    bool ethernetStarted = false;
    bool ethernetWasUp = false;
    bool stationWasConnected = false;
    bool stationEverConnected = false;
    bool reconnecting = false;
    unsigned long reconnectStartedAt = 0;

    static volatile bool timeSyncedFlag;
    bool timeSyncLogged = false;

    std::function<void(bool)> captiveCallback;
    std::function<void(Type)> activeChangeCallback;


    bool stationEnabled() const;
    bool ethernetHasIP() const;
    void startEthernet();
    void processEthernet();
    void processStation(unsigned long now);
    void processLocalAccess(unsigned long now);
    void processAp(unsigned long now);
    void startAp();
    void stopAp();
    void selectActive();
    void startTimeSync();
    static void onTimeSync(struct timeval *tv);
};

// Global instance (created in main.cpp)
extern Uplink *uplink;
