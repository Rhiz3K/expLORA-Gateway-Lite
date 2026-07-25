/**
 * expLORA Gateway Lite
 *
 * LoRa module manager implementation file
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

#include "LoRa_Module.h"
#include <Arduino.h>

// Initialization of static variables
volatile bool LoRaModule::interruptOccurred = false;

// Interrupt handler
void IRAM_ATTR LoRaModule::handleInterrupt()
{
    interruptOccurred = true;
}

// Constructor
LoRaModule::LoRaModule(Logger &log, SPIManager *spiMgr, int cs, int rst, int dio0)
    : csPin(cs), rstPin(rst), dio0Pin(dio0), spiManager(spiMgr), logger(log)
{

    // If no SPIManager instance was provided, create a new one
    if (spiManager == nullptr)
    {
        spiManager = new SPIManager(logger);
    }
}

// Destructor
LoRaModule::~LoRaModule()
{
    // We don't free the SPIManager instance if it was created externally
}

// Setup pins and SPI
bool LoRaModule::setupPins()
{
    // Set CS pin
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);

    // Set RST pin if it is set
    if (rstPin >= 0) {
        pinMode(rstPin, OUTPUT);
        digitalWrite(rstPin, HIGH);
    }

    // Set DIO0 pin
    pinMode(dio0Pin, INPUT);

    // Initialize SPI
    if (!spiManager->isInitialized())
    {
        if (!spiManager->init())
        {
            logger.error("Failed to initialize SPI manager");
            return false;
        }
    }

    // Attach interrupt to DIO0 pin
    attachInterrupt(digitalPinToInterrupt(dio0Pin), handleInterrupt, RISING);

    logger.debug("LoRa module pins configured: CS=" + String(csPin) +
                 ", RST=" + String(rstPin) + ", DIO0=" + String(dio0Pin));

    return true;
}

// Reset module
void LoRaModule::resetModule()
{
    if (rstPin < 0) {
        logger.debug("Reset pin is not available, skipping...");
        return;
    }

    logger.debug("Resetting LoRa module...");

    digitalWrite(rstPin, LOW);
    delay(10);
    digitalWrite(rstPin, HIGH);
    delay(10);
}

// Initialize LoRa module
bool LoRaModule::init()
{
    logger.info("Initializing LoRa module...");

    // Setup pins and SPI
    if (!setupPins())
    {
        logger.error("Failed to setup pins for LoRa module");
        return false;
    }

    // Reset module
    resetModule();

    // Check communication with module with retry
    uint8_t version = 0;
    uint8_t retries = 3;
    bool success = false;

    while (retries > 0 && !success)
    {
        version = getVersion();
        logger.debug("LoRa chip version: 0x" + String(version, HEX));

        if (version == 0x12)
        {
            success = true;
            break;
        }

        delay(100);
        retries--;

        // Let other tasks run between attempts
        yield();

        // Try to reset SPI and module again
        if (retries == 1)
        {
            logger.warning("Trying to restore SPI connection...");
            spiManager->reset();
            resetModule();
        }
    }

    if (!success)
    {
        logger.error("LoRa module not found after multiple attempts!");
        // Check pin states for diagnostics
        logger.debug("MISO pin state: " + String(digitalRead(SPI_MISO_PIN)));
        return false;
    }

    logger.info("Configuring LoRa module...");

    // Switch to sleep mode for configuration
    writeRegister(REG_OP_MODE, MODE_SLEEP);
    delay(10);

    // Set LoRa mode
    writeRegister(REG_OP_MODE, MODE_SLEEP | MODE_LONG_RANGE_MODE);
    delay(10);

    // Set frequency to 868 MHz for EU
    uint64_t frf = ((uint64_t)868000000 << 19) / 32000000;
    writeRegister(REG_FRF_MSB, (uint8_t)(frf >> 16));
    writeRegister(REG_FRF_MID, (uint8_t)(frf >> 8));
    writeRegister(REG_FRF_LSB, (uint8_t)(frf >> 0));

    // Set power
    writeRegister(REG_PA_CONFIG, 0x8F); // PA_BOOST, max power

    // Set receiver
    writeRegister(REG_LNA, 0x23); // High gain, automatic AGC

    // Optimization for SF7-SF10
    writeRegister(REG_DETECTION_OPTIMIZE, 0xC5);
    writeRegister(REG_DETECTION_THRESHOLD, 0x0C);

    // Over-current protection
    writeRegister(REG_OCP, 0x2F); // 150mA

    // Set base FIFO addresses
    writeRegister(REG_FIFO_TX_BASE_ADDR, 0);
    writeRegister(REG_FIFO_RX_BASE_ADDR, 0);

    // Set bandwidth, error correction, spreading factor
    writeRegister(REG_MODEM_CONFIG_1, 0x72); // BW=125kHz, CR=4/5, explicit header
    writeRegister(REG_MODEM_CONFIG_2, 0x94); // SF=9, CRC enabled (0x90 | 0x04)
    writeRegister(REG_MODEM_CONFIG_3, 0x04); // LNA AGC on

    // Set preamble
    writeRegister(REG_PREAMBLE_MSB, 0x00);
    writeRegister(REG_PREAMBLE_LSB, 0x10);

    // Set sync word
    writeRegister(REG_SYNC_WORD, 0x12);

    // Switch to continuous receive mode
    writeRegister(REG_OP_MODE, MODE_RX_CONTINUOUS | MODE_LONG_RANGE_MODE);
    delay(10);

    logger.info("LoRa module initialized and in receive mode");
    return true;
}

// Reset module
bool LoRaModule::reset()
{
    resetModule();

    // Check communication with module after reset
    uint8_t version = getVersion();
    if (version != 0x12)
    {
        logger.error("LoRa module not responding after reset");
        return false;
    }

    // Switch to sleep mode for configuration
    writeRegister(REG_OP_MODE, MODE_SLEEP);
    delay(10);

    // Switch to continuous receive mode
    writeRegister(REG_OP_MODE, MODE_RX_CONTINUOUS | MODE_LONG_RANGE_MODE);
    delay(10);

    logger.info("LoRa module reset successfully");
    return true;
}

// Write to register
void LoRaModule::writeRegister(uint8_t reg, uint8_t value)
{
    digitalWrite(csPin, LOW);

    spiManager->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    spiManager->transfer(reg | 0x80); // 0x80 indicates write
    spiManager->transfer(value);
    spiManager->endTransaction();

    digitalWrite(csPin, HIGH);
}

// Read from register
uint8_t LoRaModule::readRegister(uint8_t reg)
{
    digitalWrite(csPin, LOW);

    spiManager->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    spiManager->transfer(reg & 0x7F); // 0x7F indicates read
    uint8_t value = spiManager->transfer(0x00);
    spiManager->endTransaction();

    digitalWrite(csPin, HIGH);
    return value;
}

// Receive packet
bool LoRaModule::receivePacket(uint8_t *buffer, uint8_t *length)
{
    // Check if reception is complete
    uint8_t irqFlags = readRegister(REG_IRQ_FLAGS);

    // Check reception complete flag
    if (!(irqFlags & 0x40))
    {
        return false;
    }

    // Read number of received bytes
    uint8_t packetLength = readRegister(REG_RX_NB_BYTES);

    // Basic check of data length
    if (packetLength == 0)
    {
        logger.warning("Invalid packet length: " + String(packetLength));
        // Clear interrupt flags
        writeRegister(REG_IRQ_FLAGS, 0xFF);
        return false;
    }

    *length = packetLength;

    // Get current address in FIFO
    uint8_t currentAddr = readRegister(REG_FIFO_RX_CURRENT_ADDR);

    // Set FIFO address
    writeRegister(REG_FIFO_ADDR_PTR, currentAddr);

    // Read data from FIFO
    digitalWrite(csPin, LOW);

    spiManager->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    spiManager->transfer(REG_FIFO & 0x7F); // 0x7F indicates read

    // Transfer data
    for (int i = 0; i < packetLength; i++)
    {
        buffer[i] = spiManager->transfer(0x00);
    }

    spiManager->endTransaction();
    digitalWrite(csPin, HIGH);

    // Clear interrupt flags
    writeRegister(REG_IRQ_FLAGS, 0xFF);

    return true;
}

// Get RSSI
int LoRaModule::getRSSI()
{
    int16_t rssi = readRegister(REG_PKT_RSSI_VALUE) - 137;
    return rssi;
}

// Get SNR of last packet
float LoRaModule::getSNR()
{
    int8_t snr = readRegister(REG_PKT_SNR_VALUE);
    if (snr & 0x80)
    {
        snr = ((~snr + 1) & 0xFF) >> 2;
        snr = -snr;
    }
    else
    {
        snr = snr >> 2;
    }
    return snr * 0.25;
}

// Check if interrupt occurred
bool LoRaModule::hasInterrupt()
{
    return interruptOccurred;
}

// Clear interrupt flag
void LoRaModule::clearInterrupt()
{
    interruptOccurred = false;
}

// Check if module is connected
bool LoRaModule::isConnected()
{
    return (getVersion() == 0x12);
}

// Get chip version
uint8_t LoRaModule::getVersion()
{
    return readRegister(REG_VERSION);
}
