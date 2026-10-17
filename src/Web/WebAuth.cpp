/**
 * expLORA Gateway
 *
 * Web interface login implementation file
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

#include "WebAuth.h"
#include <esp_random.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

// Stored as "pbkdf2$<iterations>$<salt hex>$<hash hex>"
static constexpr unsigned PBKDF2_ITERATIONS = 10000;
static constexpr size_t SALT_BYTES = 16;
static constexpr size_t HASH_BYTES = 32;
static const char SESSION_COOKIE[] = "explora_session";

WebAuth::WebAuth(ConfigManager &cfg, Logger &log) : config(cfg), logger(log)
{
}

bool WebAuth::hasPassword() const
{
    return config.adminPasswordHash.length() > 0;
}

String WebAuth::randomHex(size_t bytes)
{
    String out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; i++)
    {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (uint8_t)esp_random());
        out += hex;
    }
    return out;
}

String WebAuth::hashPassword(const String &password, const String &saltHex)
{
    uint8_t hash[HASH_BYTES];
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                           reinterpret_cast<const unsigned char *>(password.c_str()), password.length(),
                                           reinterpret_cast<const unsigned char *>(saltHex.c_str()), saltHex.length(),
                                           PBKDF2_ITERATIONS, sizeof(hash), hash);
    if (rc != 0)
    {
        return String();
    }
    String out = "pbkdf2$" + String(PBKDF2_ITERATIONS) + "$" + saltHex + "$";
    for (uint8_t b : hash)
    {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", b);
        out += hex;
    }
    return out;
}

bool WebAuth::verifyPassword(const String &password) const
{
    if (!hasPassword())
    {
        return password.length() == 0;
    }
    const String &stored = config.adminPasswordHash;
    int first = stored.indexOf('$');
    int second = stored.indexOf('$', first + 1);
    int third = stored.indexOf('$', second + 1);
    if (first < 0 || second < 0 || third < 0)
    {
        return false;
    }
    String computed = hashPassword(password, stored.substring(second + 1, third));
    if (computed.length() != stored.length())
    {
        return false;
    }
    // Constant time
    uint8_t diff = 0;
    for (size_t i = 0; i < stored.length(); i++)
    {
        diff |= computed[i] ^ stored[i];
    }
    return diff == 0;
}

String WebAuth::createSession()
{
    String token = randomHex(16);
    unsigned long now = millis();
    portENTER_CRITICAL(&lock);
    // Free slot, expired slot, or the least recently used one
    size_t slot = 0;
    for (size_t i = 0; i < MAX_SESSIONS; i++)
    {
        if (sessions[i].token[0] == 0 || now - sessions[i].lastUsed > SESSION_IDLE_TIMEOUT)
        {
            slot = i;
            break;
        }
        if (sessions[i].lastUsed < sessions[slot].lastUsed)
        {
            slot = i;
        }
    }
    strncpy(sessions[slot].token, token.c_str(), sizeof(sessions[slot].token) - 1);
    sessions[slot].token[sizeof(sessions[slot].token) - 1] = 0;
    sessions[slot].lastUsed = now;
    portEXIT_CRITICAL(&lock);
    return token;
}

void WebAuth::endAllSessions()
{
    portENTER_CRITICAL(&lock);
    memset(sessions, 0, sizeof(sessions));
    portEXIT_CRITICAL(&lock);
}

String WebAuth::login(const String &password, unsigned long &retryAfterS)
{
    retryAfterS = 0;
    unsigned long now = millis();
    if (lockedUntil != 0 && (long)(lockedUntil - now) > 0)
    {
        retryAfterS = (lockedUntil - now) / 1000 + 1;
        return String();
    }
    if (password.length() > MAX_PASSWORD_LENGTH || !verifyPassword(password))
    {
        if (++failedLogins >= MAX_FAILED_LOGINS)
        {
            failedLogins = 0;
            lockedUntil = now + LOGIN_LOCK_TIME;
            retryAfterS = LOGIN_LOCK_TIME / 1000;
            logger.warning("Web login locked for " + String(LOGIN_LOCK_TIME / 1000) + " s after failed attempts");
        }
        else
        {
            logger.warning("Web login failed");
        }
        return String();
    }
    failedLogins = 0;
    lockedUntil = 0;
    logger.info("Web login");
    return createSession();
}

String WebAuth::sessionToken(AsyncWebServerRequest *request)
{
    const AsyncWebHeader *header = request->getHeader("Cookie");
    if (!header)
    {
        return String();
    }
    const String &cookies = header->value();
    String key = String(SESSION_COOKIE) + "=";
    int start = cookies.indexOf(key);
    while (start > 0 && cookies[start - 1] != ' ' && cookies[start - 1] != ';')
    {
        start = cookies.indexOf(key, start + 1);
    }
    if (start < 0)
    {
        return String();
    }
    start += key.length();
    int end = cookies.indexOf(';', start);
    return cookies.substring(start, end < 0 ? cookies.length() : end);
}

bool WebAuth::isAuthorized(AsyncWebServerRequest *request)
{
    String token = sessionToken(request);
    if (token.length() != 32)
    {
        return false;
    }
    unsigned long now = millis();
    bool valid = false;
    portENTER_CRITICAL(&lock);
    for (Session &session : sessions)
    {
        if (session.token[0] != 0 && token.equals(session.token))
        {
            if (now - session.lastUsed > SESSION_IDLE_TIMEOUT)
            {
                memset(&session, 0, sizeof(session));
            }
            else
            {
                session.lastUsed = now;
                valid = true;
            }
            break;
        }
    }
    portEXIT_CRITICAL(&lock);
    return valid;
}

void WebAuth::logout(AsyncWebServerRequest *request)
{
    String token = sessionToken(request);
    portENTER_CRITICAL(&lock);
    for (Session &session : sessions)
    {
        if (session.token[0] != 0 && token.equals(session.token))
        {
            memset(&session, 0, sizeof(session));
        }
    }
    portEXIT_CRITICAL(&lock);
}

String WebAuth::changePassword(const String &current, const String &password, const String &confirm)
{
    if (hasPassword() && !verifyPassword(current))
    {
        logger.warning("Admin password change: wrong current password");
        return "The current password is wrong";
    }
    if (password.length() < MIN_PASSWORD_LENGTH || password.length() > MAX_PASSWORD_LENGTH)
    {
        return "The password must have " + String(MIN_PASSWORD_LENGTH) + " to " + String(MAX_PASSWORD_LENGTH) +
               " characters";
    }
    if (password != confirm)
    {
        return "The passwords do not match";
    }
    String hash = hashPassword(password, randomHex(SALT_BYTES));
    if (hash.length() == 0 || !config.setAdminPasswordHash(hash))
    {
        return "Saving the password failed";
    }
    endAllSessions();
    logger.info("Admin password changed");
    return "";
}

void WebAuth::resetPassword()
{
    config.setAdminPasswordHash("");
    endAllSessions();
    logger.warning("Admin password reset from the serial console - set a new one on the next login");
}

String WebAuth::cookieFor(const String &token)
{
    String cookie = String(SESSION_COOKIE) + "=" + token + "; Path=/; HttpOnly; SameSite=Strict";
    if (token.length() == 0)
    {
        cookie += "; Max-Age=0";
    }
    return cookie;
}
