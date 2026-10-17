/**
 * expLORA Gateway
 *
 * Web interface login header file
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
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
#include <ESPAsyncWebServer.h>
#include "../Data/Logging.h"
#include "../Storage/ConfigManager.h"

/**
 * Admin password and login sessions of the web interface
 *
 * The password is stored as PBKDF2-HMAC-SHA256 with a random salt. Without a
 * stored password (fresh flash, or an update from a version without login)
 * an empty password logs in and the user must set one before anything else.
 *
 * Sessions are random tokens in an HttpOnly, SameSite=Strict cookie, kept in
 * RAM (a restart logs everyone out). Repeated failed logins lock the login
 * for a while.
 */
class WebAuth
{
public:
    static constexpr size_t MIN_PASSWORD_LENGTH = 8;
    static constexpr size_t MAX_PASSWORD_LENGTH = 64;

    WebAuth(ConfigManager &config, Logger &log);

    bool hasPassword() const;

    // Login: checks the password (empty when none is set), creates a session
    // and returns its token; empty token on failure, retryAfterS set when
    // the login is locked
    String login(const String &password, unsigned long &retryAfterS);

    // Session cookie of the request is valid (refreshes its idle timer)
    bool isAuthorized(AsyncWebServerRequest *request);

    void logout(AsyncWebServerRequest *request);

    // Set a new password; ends all sessions. Returns an error, empty on success
    String changePassword(const String &current, const String &password, const String &confirm);

    // Recovery over the serial console (physical access)
    void resetPassword();

    // "Set-Cookie" value for a session token (empty token clears the cookie)
    static String cookieFor(const String &token);

private:
    struct Session
    {
        char token[33];
        unsigned long lastUsed;
    };

    static constexpr size_t MAX_SESSIONS = 4;
    static constexpr unsigned long SESSION_IDLE_TIMEOUT = 24UL * 3600 * 1000;
    static constexpr uint8_t MAX_FAILED_LOGINS = 5;
    static constexpr unsigned long LOGIN_LOCK_TIME = 60000;

    ConfigManager &config;
    Logger &logger;

    Session sessions[MAX_SESSIONS] = {};
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    uint8_t failedLogins = 0;
    unsigned long lockedUntil = 0;

    bool verifyPassword(const String &password) const;
    static String hashPassword(const String &password, const String &saltHex);
    static String randomHex(size_t bytes);
    static String sessionToken(AsyncWebServerRequest *request);
    String createSession();
    void endAllSessions();
};
