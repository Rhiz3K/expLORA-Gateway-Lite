/**
 * expLORA Gateway
 *
 * PSRAM memory manager header file
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
#include <esp_heap_caps.h>

/**
 * Class for PSRAM memory management
 *
 * Provides methods for allocating and freeing memory in PSRAM,
 * with automatic fallback to regular RAM if PSRAM is not available.
 */
class PSRAMManager
{
private:
    static bool psramAvailable; // PSRAM availability flag

public:
    // Initialization
    static bool init();

    // Check PSRAM availability
    static bool isPSRAMAvailable();

    // Get PSRAM information
    static size_t getTotalPSRAM();
    static size_t getFreePSRAM();
    static size_t getLargestFreePSRAMBlock();

    // Allocate memory in PSRAM with fallback to heap
    static void *allocateMemory(size_t size);

    // Free memory
    static void freeMemory(void *ptr);

    // Return percentage of PSRAM usage
    static int getPSRAMUsagePercent();

    // Create buffer in PSRAM with fallback to heap
    template <typename T>
    static T *createBuffer(size_t size)
    {
        if (isPSRAMAvailable())
        {
            // Allocate in PSRAM
            T *buffer = (T *)heap_caps_malloc(sizeof(T) * size, MALLOC_CAP_SPIRAM);
            if (buffer != nullptr)
            {
                return buffer;
            }
        }

        // Fallback to regular RAM
        return new T[size];
    }

    // Free buffer
    template <typename T>
    static void deleteBuffer(T *buffer)
    {
        if (buffer == nullptr)
        {
            return;
        }

        if (isPSRAMAvailable() && heap_caps_check_integrity_addr((intptr_t)buffer, false))
        {
            // Buffer is in PSRAM
            heap_caps_free(buffer);
        }
        else
        {
            // Buffer is in regular RAM
            delete[] buffer;
        }
    }
};
