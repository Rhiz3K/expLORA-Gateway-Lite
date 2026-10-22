/**
 * expLORA Gateway
 *
 * Power source detection implementation file
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

#include "PowerSource.h"

static constexpr unsigned long POWER_SOURCE_POLL = 1000;

void PowerSource::begin()
{
#if defined(HAS_PWR_SRC)
    pinMode(PWR_SRC_PIN, INPUT);
    last = read();
    logger.info(String("Power source: ") + getName());
#endif
}

void PowerSource::process()
{
#if defined(HAS_PWR_SRC)
    unsigned long now = millis();
    if (now - lastRead < POWER_SOURCE_POLL)
    {
        return;
    }
    lastRead = now;
    int current = read();
    if (current != last)
    {
        last = current;
        logger.warning(String("Power source changed: ") + getName());
    }
#endif
}

bool PowerSource::available()
{
#if defined(HAS_PWR_SRC)
    return true;
#else
    return false;
#endif
}

const char *PowerSource::getName() const
{
#if defined(HAS_PWR_SRC)
    return last == HIGH ? "PoE" : "USB";
#else
    return "";
#endif
}

int PowerSource::read() const
{
#if defined(HAS_PWR_SRC)
    return digitalRead(PWR_SRC_PIN);
#else
    return -1;
#endif
}
