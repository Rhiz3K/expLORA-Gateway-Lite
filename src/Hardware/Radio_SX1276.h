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

#include "board_config.h"

#if defined(RADIO_SX1276)

#include <Arduino.h>
#include <RadioLib.h>
#include "../config.h"
#include "../Data/Logging.h"
#include "SPI_Manager.h"
#include "LoRaRadio.h"

/**
 * SX1276 (RFM95W) LoRa radio driver (RadioLib)
 *
 * Receive only: the driver never calls transmit. DIO0 signals RxDone;
 * packets with a CRC error are rejected.
 */
class RadioSX1276 : public LoRaRadio
{
private:
    Logger &logger;
    SPIManager *spiManager;
    Module *module;
    SX1276 *radio;
    bool initialized;

    // Configure the chip and start continuous receive
    bool configure();

    // Log a RadioLib error code
    void logError(const char *what, int16_t state);

public:
    RadioSX1276(Logger &log, SPIManager *spiMgr);
    ~RadioSX1276() override;

    bool init() override;
    bool reset() override;
    bool receivePacket(uint8_t *buffer, uint8_t *length) override;
    int getRSSI() override;
    float getSNR() override;
    bool isConnected() override;
    const char *getChipName() const override { return "SX1276"; }
};

#endif // RADIO_SX1276
