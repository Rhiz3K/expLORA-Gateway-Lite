/**
 * expLORA Gateway
 *
 * LoRa radio interface implementation file
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

#include "LoRaRadio.h"
#include "board_config.h"

#if defined(RADIO_SX1276)
#include "Radio_SX1276.h"
#elif defined(RADIO_SX1262)
#include "Radio_SX1262.h"
#else
#error "board_config.h must define RADIO_SX1276 or RADIO_SX1262"
#endif

// Initialization of static variables
volatile bool LoRaRadio::interruptOccurred = false;

// Interrupt handler
void IRAM_ATTR LoRaRadio::handleInterrupt()
{
    interruptOccurred = true;
}

// Check if interrupt occurred
bool LoRaRadio::hasInterrupt()
{
    return interruptOccurred;
}

// Clear interrupt flag
void LoRaRadio::clearInterrupt()
{
    interruptOccurred = false;
}

// Create the radio driver selected in board_config.h
LoRaRadio *createLoRaRadio(Logger &logger, SPIManager *spiManager)
{
#if defined(RADIO_SX1276)
    return new RadioSX1276(logger, spiManager);
#else
    return new RadioSX1262(logger, spiManager);
#endif
}
