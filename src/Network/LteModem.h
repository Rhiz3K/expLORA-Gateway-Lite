/**
 * expLORA Gateway
 *
 * LTE modem (PPP over UART) header file
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
#include "../config.h"
#include "../Data/Logging.h"
#include "../Storage/ConfigManager.h"

#if defined(HAS_LTE)

/**
 * LTE modem (SIMCom A7682E) driven through the core's PPP interface
 *
 * Non-blocking state machine, call process() from loop():
 *   Off -> Booting (power on, wait for AT) -> Starting (PPP driver, SIM)
 *   -> Registering (wait for network) -> Connecting (CMUX, PPP) -> Online
 * Failures end in PowerCycle (AT+CPOF, power off, retry later).
 *
 * The PPP netif keeps its default route priority (20), below ethernet and
 * WiFi, so the session is a hot standby used only when nothing else is up.
 */
class LteModem
{
public:
    enum class State : uint8_t
    {
        Disabled,
        Off,
        Booting,
        Starting,
        Registering,
        Connecting,
        Online,
        PowerCycle,
    };

    LteModem(ConfigManager &config, Logger &log);

    void begin();
    void process();

    State getState() const { return state; }
    const char *getStateName() const;
    bool hasIP() const;
    IPAddress localIP() const;

    // Cached diagnostics, refreshed while registered
    int getSignalQuality() const { return csq; } // 0..31, 99 = unknown
    int getRssiDbm() const;                      // 0 = unknown
    const String &getOperator() const { return operatorName; }
    const String &getModuleName() const { return moduleName; }
    const String &getImei() const { return imei; }
    const String &getLastError() const { return lastError; }

private:
    ConfigManager &config;
    Logger &logger;

    State state = State::Disabled;
    unsigned long stateSince = 0;
    unsigned long lastPoll = 0;
    uint8_t failures = 0;
    bool pppStarted = false;
    bool atUartOpen = false; // raw AT access before the PPP driver owns the UART
    String rxBuffer;

    int csq = 99;
    String operatorName;
    String moduleName;
    String imei;
    String lastError;

    void setState(State next);
    void fail(const String &reason);
    void powerOn();
    void powerOff();
    bool probeAt(unsigned long now);
    void startPpp();
    void stopPpp();
    void refreshSignal();
};

#endif
