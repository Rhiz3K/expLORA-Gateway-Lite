/**
 * expLORA Gateway Lite
 *
 * PSRAM memory manager implementation file
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

#include "PSRAM_Manager.h"
#include <Arduino.h>

// Initialization of static variables
bool PSRAMManager::psramAvailable = false;

// Initialization
bool PSRAMManager::init()
{
    psramAvailable = psramFound();

    if (psramAvailable)
    {
        Serial.printf("PSRAM initialized: %u bytes total, %u bytes free\n",
                      getTotalPSRAM(), getFreePSRAM());
    }
    else
    {
        Serial.println("PSRAM not available");
    }

    return psramAvailable;
}

// Check PSRAM availability
bool PSRAMManager::isPSRAMAvailable()
{
    return psramAvailable;
}

// Get PSRAM information
size_t PSRAMManager::getTotalPSRAM()
{
    if (!psramAvailable)
        return 0;
    return ESP.getPsramSize();
}

size_t PSRAMManager::getFreePSRAM()
{
    if (!psramAvailable)
        return 0;
    return ESP.getFreePsram();
}

size_t PSRAMManager::getLargestFreePSRAMBlock()
{
    if (!psramAvailable)
        return 0;
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

// Allocate memory in PSRAM with fallback to heap
void *PSRAMManager::allocateMemory(size_t size)
{
    void *ptr = nullptr;

    if (psramAvailable)
    {
        // Try to allocate in PSRAM
        ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }

    if (ptr == nullptr)
    {
        // Fallback to regular RAM
        ptr = malloc(size);
    }

    return ptr;
}

// Free memory
void PSRAMManager::freeMemory(void *ptr)
{
    if (ptr == nullptr)
    {
        return;
    }

    if (psramAvailable && heap_caps_check_integrity_addr((intptr_t)ptr, false))
    {
        // Memory is in PSRAM
        heap_caps_free(ptr);
    }
    else
    {
        // Memory is in regular RAM
        free(ptr);
    }
}

// Return percentage of PSRAM usage
int PSRAMManager::getPSRAMUsagePercent()
{
    if (!psramAvailable)
        return 0;

    size_t total = getTotalPSRAM();
    size_t free = getFreePSRAM();

    if (total == 0)
        return 0;

    return 100 - (free * 100 / total);
}
