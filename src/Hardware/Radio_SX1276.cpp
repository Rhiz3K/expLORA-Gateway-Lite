/**
 * expLORA Gateway
 *
 * SX1276 (RFM95W) LoRa radio driver implementation file
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

#include "Radio_SX1276.h"

#if defined(RADIO_SX1276)

// Constructor
RadioSX1276::RadioSX1276(Logger &log, SPIManager *spiMgr)
    : logger(log), spiManager(spiMgr), module(nullptr), radio(nullptr), initialized(false)
{
    if (spiManager == nullptr)
    {
        spiManager = new SPIManager(logger);
    }
}

// Destructor
RadioSX1276::~RadioSX1276()
{
    delete radio;
    delete module;
}

// Log a RadioLib error code
void RadioSX1276::logError(const char *what, int16_t state)
{
    logger.error(String("SX1276 ") + what + " failed, RadioLib error " + String(state));
}

// Configure the chip and start continuous receive
bool RadioSX1276::configure()
{
    // TX power is irrelevant (receive only), RadioLib needs a value; gain 0 = AGC
    int16_t state = radio->begin(LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                                 LORA_SYNC_WORD, 10, LORA_PREAMBLE_LENGTH, 0);
    if (state != RADIOLIB_ERR_NONE)
    {
        logError("begin", state);
        return false;
    }

    state = radio->setCRC(true);
    if (state != RADIOLIB_ERR_NONE)
    {
        logError("setCRC", state);
        return false;
    }

    radio->setPacketReceivedAction(handleInterrupt);

    state = radio->startReceive();
    if (state != RADIOLIB_ERR_NONE)
    {
        logError("startReceive", state);
        return false;
    }

    return true;
}

// Initialize LoRa module
bool RadioSX1276::init()
{
    logger.info("Initializing LoRa module (SX1276)...");

    if (!spiManager->isInitialized() && !spiManager->init())
    {
        logger.error("Failed to initialize SPI manager");
        return false;
    }

    if (radio == nullptr)
    {
        module = new Module(LORA_CS, LORA_DIO0, LORA_RST, RADIOLIB_NC, *spiManager->getSPI(),
                            SPISettings(2000000, MSBFIRST, SPI_MODE0));
        radio = new SX1276(module);
    }

    initialized = configure();
    if (initialized)
    {
        logger.info("LoRa module initialized and in receive mode");
    }
    return initialized;
}

// Reset module
bool RadioSX1276::reset()
{
    if (radio == nullptr)
    {
        return init();
    }

    radio->clearPacketReceivedAction();
    radio->reset();
    initialized = configure();
    if (initialized)
    {
        logger.info("LoRa module reset successfully");
    }
    return initialized;
}

// Receive packet
bool RadioSX1276::receivePacket(uint8_t *buffer, uint8_t *length)
{
    if (!initialized)
    {
        return false;
    }

    size_t packetLength = radio->getPacketLength();
    int16_t state = radio->readData(buffer, packetLength);

    // Make sure the chip is back in continuous receive
    radio->startReceive();

    if (state == RADIOLIB_ERR_CRC_MISMATCH)
    {
        logger.warning("LoRa packet CRC error, dropped");
        return false;
    }
    if (state != RADIOLIB_ERR_NONE)
    {
        logger.warning("LoRa packet read failed, RadioLib error " + String(state));
        return false;
    }
    if (packetLength == 0 || packetLength > 255)
    {
        logger.warning("Invalid packet length: " + String(packetLength));
        return false;
    }

    *length = (uint8_t)packetLength;
    return true;
}

// Get RSSI of last packet (RadioLib applies the HF port offset and the SNR
// correction below the noise floor, SX1276 datasheet 5.5.5)
int RadioSX1276::getRSSI()
{
    return initialized ? (int)radio->getRSSI() : 0;
}

// Get SNR of last packet
float RadioSX1276::getSNR()
{
    return initialized ? radio->getSNR() : 0.0f;
}

// Check if module is connected (silicon version register reads 0x12)
bool RadioSX1276::isConnected()
{
    if (!initialized)
    {
        return false;
    }
    return module->SPIreadRegister(RADIOLIB_SX127X_REG_VERSION) == 0x12;
}

#endif // RADIO_SX1276
