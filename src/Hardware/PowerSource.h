/**
 * expLORA Gateway
 *
 * Power source detection header file
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

/**
 * Supply source of boards with a power multiplexer (HAS_PWR_SRC)
 *
 * The Pro board feeds +5V either from PoE or from USB through a TPS2116; its
 * status output is wired to PWR_SRC_PIN (H = PoE, L = USB).
 */
class PowerSource
{
public:
    explicit PowerSource(Logger &log) : logger(log) {}

    void begin();

    // Log changes; call from loop()
    void process();

    static bool available();
    // "PoE", "USB", or "" on boards without detection
    const char *getName() const;

private:
    Logger &logger;
    int last = -1;
    unsigned long lastRead = 0;

    int read() const;
};

extern PowerSource *powerSource;
