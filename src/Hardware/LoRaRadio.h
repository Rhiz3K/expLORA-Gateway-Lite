/**
 * expLORA Gateway
 *
 * LoRa radio interface - common API for the supported radio chips
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
#include "../Data/Logging.h"
#include "SPI_Manager.h"

/**
 * Abstract LoRa radio
 *
 * The gateway only receives. Each driver configures the chip with the
 * LORA_* parameters from config.h and stays in continuous receive mode.
 * The chip's "packet ready" pin sets a shared interrupt flag.
 */
class LoRaRadio
{
protected:
    static volatile bool interruptOccurred;  // Interrupt flag (static for use in ISR)
    static void IRAM_ATTR handleInterrupt(); // Interrupt handler

public:
    virtual ~LoRaRadio() = default;

    // Initialize the chip and start continuous receive
    virtual bool init() = 0;

    // Reset the chip and restart receive
    virtual bool reset() = 0;

    // Read a received packet; false if none is ready or it is invalid
    virtual bool receivePacket(uint8_t *buffer, uint8_t *length) = 0;

    // RSSI of the last packet in dBm
    virtual int getRSSI() = 0;

    // SNR of the last packet in dB
    virtual float getSNR() = 0;

    // Check if the chip responds
    virtual bool isConnected() = 0;

    // Chip name for logs and the web UI
    virtual const char *getChipName() const = 0;

    // Check if interrupt occurred
    static bool hasInterrupt();

    // Clear interrupt flag
    static void clearInterrupt();
};

// Create the radio driver selected in board_config.h
LoRaRadio *createLoRaRadio(Logger &logger, SPIManager *spiManager);
