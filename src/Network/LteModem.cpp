/**
 * expLORA Gateway
 *
 * LTE modem (PPP over UART) implementation file
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

#include "LteModem.h"

#if defined(HAS_LTE)

#include <PPP.h>
#include <esp_task_wdt.h>

// UART used for the modem (UART0 is the console, UART1 is free but its default
// pins collide with the flash on WROOM modules)
static constexpr uint8_t LTE_UART = 2;
static constexpr int LTE_BAUD = 115200;

// The modem powers up on its own once +5V_LTE is switched on (PWRKEY is
// held active by hardware); it answers AT after ~10-15 s
static constexpr unsigned long LTE_BOOT_TIMEOUT = 40000;
static constexpr unsigned long LTE_AT_PROBE_INTERVAL = 1000;
static constexpr unsigned long LTE_REGISTER_TIMEOUT = 180000;
static constexpr unsigned long LTE_REGISTER_POLL = 3000;
static constexpr unsigned long LTE_CONNECT_TIMEOUT = 60000;
static constexpr unsigned long LTE_SIGNAL_POLL = 30000;
// Lost PPP session this long while online counts as a failure
static constexpr unsigned long LTE_LOST_IP_GRACE = 30000;
// AT+CPOF needs a few seconds before the supply may be cut
static constexpr unsigned long LTE_POWER_DOWN_DELAY = 3000;
// Retry back-off after a failure: 30 s, 60 s, ... up to 10 min
static constexpr unsigned long LTE_RETRY_BASE = 30000;
static constexpr unsigned long LTE_RETRY_MAX = 600000;

LteModem::LteModem(ConfigManager &cfg, Logger &log)
    : config(cfg), logger(log)
{
}

void LteModem::begin()
{
    // Supply off until enabled; UART pins high-impedance so the unpowered
    // modem is not back-fed through the level shifter
    pinMode(LTE_PWR_PIN, OUTPUT);
    digitalWrite(LTE_PWR_PIN, LOW);
    pinMode(LTE_TX_PIN, INPUT);
    pinMode(LTE_RX_PIN, INPUT);
    pinMode(LTE_NET_PIN, INPUT);

    if (!config.lteEnabled)
    {
        logger.info("LTE disabled in configuration");
        setState(State::Disabled);
        return;
    }
    if (config.lteApn.length() == 0)
    {
        logger.warning("LTE enabled but no APN configured - LTE stays off");
        lastError = "No APN configured";
        setState(State::Disabled);
        return;
    }
    setState(State::Off);
}

void LteModem::process()
{
    unsigned long now = millis();
    unsigned long inState = now - stateSince;

    switch (state)
    {
    case State::Disabled:
        break;

    case State::Off:
        powerOn();
        setState(State::Booting);
        break;

    case State::Booting:
        if (probeAt(now))
        {
            logger.info("LTE modem responding after " + String(inState / 1000) + " s");
            startPpp();
        }
        else if (inState > LTE_BOOT_TIMEOUT)
        {
            fail("Modem does not respond to AT");
        }
        break;

    case State::Starting:
        // startPpp() moves on or fails synchronously
        break;

    case State::Registering:
        if (now - lastPoll < LTE_REGISTER_POLL)
        {
            break;
        }
        lastPoll = now;
        if (PPP.attached())
        {
            refreshSignal();
            operatorName = PPP.operatorName();
            logger.info("LTE registered: " + operatorName + ", CSQ " + String(csq) +
                        " (" + String(getRssiDbm()) + " dBm), switching to data mode");
            setState(State::Connecting);
            if (!PPP.mode(ESP_MODEM_MODE_CMUX))
            {
                fail("CMUX mode failed");
            }
        }
        else if (inState > LTE_REGISTER_TIMEOUT)
        {
            refreshSignal();
            fail("Network registration timeout (CSQ " + String(csq) + ")");
        }
        break;

    case State::Connecting:
        if (PPP.hasIP())
        {
            logger.info("LTE connected, IP: " + PPP.localIP().toString());
            failures = 0;
            lastError = "";
            setState(State::Online);
            lastPoll = now;
        }
        else if (inState > LTE_CONNECT_TIMEOUT)
        {
            fail("PPP connection timeout");
        }
        break;

    case State::Online:
        if (!PPP.hasIP())
        {
            // stateSince marks when the session was lost
            if (lastPoll != 0)
            {
                logger.warning("LTE connection lost");
                lastPoll = 0;
                stateSince = now;
            }
            else if (inState > LTE_LOST_IP_GRACE)
            {
                fail("PPP connection lost");
            }
            break;
        }
        if (lastPoll == 0)
        {
            logger.info("LTE connection restored, IP: " + PPP.localIP().toString());
            lastPoll = now;
        }
        if (now - lastPoll > LTE_SIGNAL_POLL)
        {
            lastPoll = now;
            refreshSignal();
        }
        break;

    case State::PowerCycle:
    {
        unsigned long backoff = min(LTE_RETRY_BASE * failures, LTE_RETRY_MAX);
        if (inState > backoff)
        {
            logger.info("LTE retrying (attempt " + String(failures + 1) + ")");
            setState(State::Off);
        }
        break;
    }
    }
}

void LteModem::setState(State next)
{
    state = next;
    stateSince = millis();
    lastPoll = 0;
}

void LteModem::fail(const String &reason)
{
    failures++;
    lastError = reason;
    logger.warning("LTE: " + reason + ", powering off");
    stopPpp();
    powerOff();
    setState(State::PowerCycle);
}

void LteModem::powerOn()
{
    pinMode(LTE_RX_PIN, INPUT_PULLUP);
    Serial2.begin(LTE_BAUD, SERIAL_8N1, LTE_RX_PIN, LTE_TX_PIN);
    atUartOpen = true;
    digitalWrite(LTE_PWR_PIN, HIGH);
    rxBuffer = "";
    logger.info("LTE modem powered on");
}

void LteModem::powerOff()
{
    if (atUartOpen)
    {
        // Still in plain AT mode (boot failed) - power down cleanly
        Serial2.print("AT+CPOF\r");
        Serial2.flush();
        delay(LTE_POWER_DOWN_DELAY);
        Serial2.end();
        atUartOpen = false;
    }
    digitalWrite(LTE_PWR_PIN, LOW);
    pinMode(LTE_TX_PIN, INPUT);
    pinMode(LTE_RX_PIN, INPUT);
    csq = 99;
    operatorName = "";
}

// Send AT once per interval on the raw UART, true when OK came back
bool LteModem::probeAt(unsigned long now)
{
    while (Serial2.available())
    {
        rxBuffer += (char)Serial2.read();
        if (rxBuffer.length() > 128)
        {
            rxBuffer.remove(0, 64);
        }
    }
    if (rxBuffer.indexOf("OK") >= 0)
    {
        return true;
    }
    if (now - lastPoll > LTE_AT_PROBE_INTERVAL)
    {
        lastPoll = now;
        rxBuffer = "";
        Serial2.print("AT\r");
    }
    return false;
}

// Hand the UART to the PPP driver: SIM check, module info
void LteModem::startPpp()
{
    setState(State::Starting);

    // Echo off for the driver's response parsing
    Serial2.print("ATE0\r");
    Serial2.flush();
    delay(200);
    Serial2.end();
    atUartOpen = false;

    PPP.setApn(config.lteApn.c_str());
    PPP.setPin(config.ltePin.length() > 0 ? config.ltePin.c_str() : nullptr);
    PPP.setPins(LTE_TX_PIN, LTE_RX_PIN);
    if (!PPP.begin(PPP_MODEM_SIM7600, LTE_UART, LTE_BAUD))
    {
        // begin() cleans up after itself
        fail("PPP driver start failed (SIM / PIN?)");
        return;
    }
    pppStarted = true;

    // LTE attach already uses PDP context 1, so it must carry the configured
    // APN before registration (the driver sets it only when entering data mode)
    String response;
    if (!PPP.cmd("AT+CGDCONT=1,\"IP\",\"" + config.lteApn + "\"", response, 3000))
    {
        logger.warning("LTE: setting APN failed: " + response);
    }

    moduleName = PPP.moduleName();
    imei = PPP.IMEI();
    logger.info("LTE modem " + moduleName + ", IMEI " + imei);
    setState(State::Registering);
}

void LteModem::stopPpp()
{
    if (!pppStarted)
    {
        return;
    }
    // Modem commands and driver teardown can each take seconds (command
    // timeouts, CMUX close) - keep the task watchdog out of it
    bool watched = esp_task_wdt_status(NULL) == ESP_OK;
    if (watched)
    {
        esp_task_wdt_delete(NULL);
    }

    // AT+CPOF; works in command and CMUX mode
    if (PPP.mode() == ESP_MODEM_MODE_DATA || !PPP.powerDown())
    {
        logger.debug("LTE power down command failed");
    }
    delay(LTE_POWER_DOWN_DELAY);
    PPP.end();
    pppStarted = false;

    if (watched)
    {
        esp_task_wdt_add(NULL);
    }
}

void LteModem::refreshSignal()
{
    int q = PPP.RSSI();
    csq = q < 0 ? 99 : q;
}

bool LteModem::hasIP() const
{
    return state == State::Online && PPP.hasIP();
}

IPAddress LteModem::localIP() const
{
    return hasIP() ? PPP.localIP() : IPAddress();
}

// 3GPP 27.007: 0 = -113 dBm ... 31 = -51 dBm, 99 = unknown
int LteModem::getRssiDbm() const
{
    return (csq >= 0 && csq <= 31) ? -113 + 2 * csq : 0;
}

const char *LteModem::getStateName() const
{
    switch (state)
    {
    case State::Disabled:
        return "disabled";
    case State::Off:
        return "off";
    case State::Booting:
        return "booting";
    case State::Starting:
        return "starting";
    case State::Registering:
        return "registering";
    case State::Connecting:
        return "connecting";
    case State::Online:
        return "online";
    case State::PowerCycle:
        return "waiting to retry";
    }
    return "unknown";
}

#endif
