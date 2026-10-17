/**
 * expLORA Gateway
 *
 * SX1276 (RFM95W) LoRa radio driver header file
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
#include <SPI.h>
#include "../config.h"
#include "../Data/Logging.h"
#include "SPI_Manager.h"
#include "LoRaRadio.h"
#include "board_config.h"

#if defined(RADIO_SX1276)

/**
 * SX1276 (RFM95W) LoRa radio driver
 *
 * Handles initialization, configuration, and communication with RFM95W LoRa module.
 * Abstracts SPI communication; DIO0 signals RxDone.
 */
class RadioSX1276 : public LoRaRadio
{
private:
    int csPin;              // Chip select pin
    int rstPin;             // Reset pin
    int dio0Pin;            // DIO0 pin for interrupt
    SPIManager *spiManager; // SPI communication manager

    Logger &logger; // Reference to logger

    // Setup pins and SPI
    bool setupPins();
    // Reset module
    void resetModule();

public:
    // Constructor
    RadioSX1276(Logger &log, SPIManager *spiMgr = nullptr, int cs = LORA_CS, int rst = LORA_RST, int dio0 = LORA_DIO0);

    // Destructor
    ~RadioSX1276() override;

    // Initialize LoRa module
    bool init() override;

    // Reset module
    bool reset() override;

    // Write to register
    void writeRegister(uint8_t reg, uint8_t value);

    // Read from register
    uint8_t readRegister(uint8_t reg);

    // Receive packet
    bool receivePacket(uint8_t *buffer, uint8_t *length) override;

    // Get RSSI
    int getRSSI() override;

    // Return SNR of last packet
    float getSNR() override;

    // Check if module is connected
    bool isConnected() override;

    // Get chip version
    uint8_t getVersion();

    const char *getChipName() const override { return "SX1276"; }
};

#endif // RADIO_SX1276
