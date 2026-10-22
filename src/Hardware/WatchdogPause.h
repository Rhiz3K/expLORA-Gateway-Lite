/**
 * expLORA Gateway
 *
 * Task watchdog pause helper
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

#include <esp_task_wdt.h>

/**
 * Takes the calling task off the task watchdog for the lifetime of the
 * object - for calls that legitimately block longer than WDT_TIMEOUT
 * (DNS, TLS handshakes, modem commands). Nested pauses are harmless.
 */
class WatchdogPause
{
public:
    WatchdogPause() : watched(esp_task_wdt_status(NULL) == ESP_OK)
    {
        if (watched)
        {
            esp_task_wdt_delete(NULL);
        }
    }
    ~WatchdogPause()
    {
        if (watched)
        {
            esp_task_wdt_add(NULL);
        }
    }
    WatchdogPause(const WatchdogPause &) = delete;
    WatchdogPause &operator=(const WatchdogPause &) = delete;

private:
    bool watched;
};
