#include "WebServer.h"

#include "HTMLGenerator.h"
#include "../Security/OtaTrust.h"
#include "../Security/SecurityPolicy.h"
#include "../config.h"
#include "../Hardware/WatchdogPause.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr size_t MAX_MANIFEST_BYTES = 8192;
constexpr uint32_t OTA_STALL_TIMEOUT_MS = 15000;
constexpr uint32_t OTA_DOWNLOAD_TIMEOUT_MS = 900000;

const char *firmwareImageError(const uint8_t *data, size_t length)
{
    if (length < sizeof(esp_image_header_t)) return "Firmware image too short";
    esp_image_header_t header;
    memcpy(&header, data, sizeof(header));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC) return "Not an ESP32 firmware image";
    if (header.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID)
        return "Firmware is for a different board (" BOARD_NAME " expected)";
    return nullptr;
}

String httpsBlocker()
{
    if (time(nullptr) < 1700000000)
        return "The clock is not synchronized yet (needed to check certificates)";
    if (ESP.getFreeHeap() < 48000 || ESP.getMaxAllocHeap() < 24000)
        return "Not enough free memory for HTTPS";
    return "";
}

struct FirmwareUploadState
{
    bool error;
    int httpStatus;
    const char *errorMessage;
    char expectedSha256[65];
    size_t written;
    size_t flashed;
    uint8_t header[sizeof(esp_image_header_t)];
    size_t headerLength;
    bool ownsOta;
    bool updateBegun;
    bool shaInitialized;
    bool installed;
    mbedtls_sha256_context shaContext;
};

struct OtaTaskContext
{
    WebPortal *portal;
    String url;
    String version;
    String expectedSha256;
};

// HTTPClient::getString() grows an Arduino String until a chunked response
// ends. Write through this bounded sink instead so a malformed official
// response cannot consume the gateway heap before the size check runs.
class BoundedStringStream : public Stream
{
public:
    explicit BoundedStringStream(size_t limit) : limit(limit) {}

    size_t write(const uint8_t *buffer, size_t size) override
    {
        if (buffer == nullptr || size == 0)
        {
            return 0;
        }
        if (size > limit - value.length())
        {
            exceeded = true;
            setWriteError();
            return 0;
        }
        if (!value.concat(reinterpret_cast<const char *>(buffer), size))
        {
            setWriteError();
            return 0;
        }
        return size;
    }

    size_t write(uint8_t byte) override
    {
        return write(&byte, 1);
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    const String &payload() const { return value; }
    bool limitExceeded() const { return exceeded; }

private:
    const size_t limit;
    String value;
    bool exceeded = false;
};

String digestToHex(const uint8_t digest[32])
{
    static const char hex[] = "0123456789abcdef";
    char output[65];
    for (size_t index = 0; index < 32; ++index)
    {
        output[index * 2] = hex[(digest[index] >> 4) & 0x0f];
        output[index * 2 + 1] = hex[digest[index] & 0x0f];
    }
    output[64] = '\0';
    return String(output);
}

String requestHeader(AsyncWebServerRequest *request, const char *name)
{
    if (!request->hasHeader(name))
    {
        return "";
    }
    const AsyncWebHeader *header = request->getHeader(name);
    if (header == nullptr)
    {
        return "";
    }
    String value = header->value();
    value.trim();
    return value;
}
}

void WebPortal::handleFirmwareCheck(AsyncWebServerRequest *request)
{
    if (!authorizeRequest(request, false)) return;
    int status = 202;
    String body = "{\"status\":\"checking\"}";
    {
        std::lock_guard<std::mutex> guard(otaWorkMutex);
        if (updateCheckDone && millis() - updateCheckAt < 60000)
        {
            status = updateCheckStatus;
            body = updateCheckResult;
            updateCheckDone = false;
        }
        else if (!updateCheckRunning)
        {
            updateCheckRequested = true;
            updateCheckRunning = true;
        }
    }
    request->send(status, "application/json", body);
}

void WebPortal::runUpdateCheck()
{
    int status = 500;
    String body;
    fetchFirmwareManifest(status, body);
    std::lock_guard<std::mutex> guard(otaWorkMutex);
    updateCheckStatus = status;
    updateCheckResult = body;
    updateCheckDone = true;
    updateCheckRunning = false;
    updateCheckAt = millis();
}

void WebPortal::fetchFirmwareManifest(int &status, String &result)
{
    logger.debug("Fetching official firmware manifest");
    if (!SecurityPolicy::isAllowedManifestUrl(FIRMWARE_UPDATE_URL, FIRMWARE_UPDATE_URL))
    {
        logger.error("Built-in firmware manifest URL violates OTA policy");
        status = 500;
        result = "{\"error\":\"Invalid built-in firmware manifest URL\"}";
        return;
    }

    String blocker = httpsBlocker();
    if (blocker.length() > 0 && time(nullptr) >= 1700000000 && mqttManager && mqttManager->isConnected() && mqttManager->usesTls())
    {
        mqttManager->disconnect();
        blocker = httpsBlocker();
    }
    if (blocker.length() > 0)
    {
        status = 503;
        result = "{\"error\":\"" + blocker + "\"}";
        return;
    }
    WatchdogPause pause;
    WiFiClientSecure tlsClient;
    configureOtaTls(tlsClient);
    HTTPClient http;
    http.setTimeout(5000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(tlsClient, FIRMWARE_UPDATE_URL))
    {
        status = 500;
        result = "{\"error\":\"Firmware check TLS initialization failed\"}";
        return;
    }
    http.addHeader("User-Agent", "expLORA-Gateway/" + String(FIRMWARE_VERSION));

    const int httpCode = http.GET();
    const int contentLength = http.getSize();
    if (httpCode != HTTP_CODE_OK || contentLength > static_cast<int>(MAX_MANIFEST_BYTES))
    {
        logger.error("Firmware manifest rejected: HTTP " + String(httpCode));
        http.end();
        status = 502;
        result = "{\"error\":\"Official firmware manifest is unavailable or invalid\"}";
        return;
    }

    BoundedStringStream manifestStream(MAX_MANIFEST_BYTES);
    const int manifestBytes = http.writeToStream(&manifestStream);
    http.end();
    if (manifestBytes <= 0 || manifestStream.limitExceeded() ||
        manifestStream.payload().length() > MAX_MANIFEST_BYTES)
    {
        status = 502;
        result = "{\"error\":\"Official firmware manifest size is invalid\"}";
        return;
    }

    DynamicJsonDocument incoming(MAX_MANIFEST_BYTES);
    const DeserializationError parseError =
        deserializeJson(incoming, manifestStream.payload());
    if (parseError)
    {
        status = 502;
        result = "{\"error\":\"Official firmware manifest is not valid JSON\"}";
        return;
    }

    String version = incoming["version"] | "";
    String url = incoming["url"] | "";
    String sha256 = incoming["sha256"] | "";
    version.trim();
    url.trim();
    sha256.trim();
    sha256.toLowerCase();

    bool installable = true;
    String reason;
    if (!SecurityPolicy::isVersionNewer(version.c_str(), FIRMWARE_VERSION))
    {
        installable = false;
        reason = "No newer semantic firmware version is available";
    }
    else if (!SecurityPolicy::isAllowedFirmwareUrl(url.c_str(), FIRMWARE_UPDATE_URL))
    {
        installable = false;
        reason = "Manifest firmware URL is outside the official HTTPS origin";
    }
    else if (!SecurityPolicy::isHexString(sha256.c_str(), 64))
    {
        installable = false;
        reason = "Manifest firmware SHA-256 digest is invalid";
    }

    DynamicJsonDocument responseDocument(1024);
    responseDocument["version"] = version;
    responseDocument["url"] = url;
    responseDocument["sha256"] = sha256;
    responseDocument["currentVersion"] = FIRMWARE_VERSION;
    responseDocument["installable"] = installable;
    if (!installable)
    {
        responseDocument["reason"] = reason;
    }
    String response;
    serializeJson(responseDocument, response);
    logger.info("Firmware check completed; installable=" +
                String(installable ? "true" : "false"));
    status = 200;
    result = response;
}

void WebPortal::handleFirmwareUpdate(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /firmware/update");
    if (!authorizeRequest(request, true)) return;

    static const char *required[] = {"url", "version", "sha256"};
    for (const char *name : required)
    {
        if (!request->hasParam(name, true))
        {
            request->send(400, "application/json",
                          "{\"error\":\"Firmware metadata is incomplete\"}");
            return;
        }
    }

    OtaTaskContext *context = new (std::nothrow) OtaTaskContext{
        this,
        request->getParam("url", true)->value(),
        request->getParam("version", true)->value(),
        request->getParam("sha256", true)->value()};
    if (context == nullptr)
    {
        request->send(503, "application/json",
                      "{\"error\":\"Insufficient memory to start firmware update\"}");
        return;
    }
    context->url.trim();
    context->version.trim();
    context->expectedSha256.trim();
    context->expectedSha256.toLowerCase();

    String verificationError;
    if (!SecurityPolicy::isAllowedFirmwareUrl(context->url.c_str(), FIRMWARE_UPDATE_URL))
    {
        verificationError = "Firmware URL is outside the official HTTPS origin";
    }
    else if (!SecurityPolicy::isVersionNewer(context->version.c_str(),
                                              FIRMWARE_VERSION))
    {
        verificationError = "Firmware version must be newer than the running version";
    }
    else if (!SecurityPolicy::isHexString(
                 context->expectedSha256.c_str(), 64))
    {
        verificationError = "Invalid firmware SHA-256 digest";
    }

    if (!verificationError.isEmpty())
    {
        DynamicJsonDocument document(256);
        document["error"] = verificationError;
        String response;
        serializeJson(document, response);
        delete context;
        request->send(400, "application/json", response);
        return;
    }

    if (!tryBeginOta(context))
    {
        delete context;
        request->send(409, "application/json",
                      "{\"error\":\"Another OTA operation is already in progress\"}");
        return;
    }

    {
        std::lock_guard<std::mutex> guard(otaWorkMutex);
        pendingOtaContext = context;
    }

    request->send(202, "application/json",
                  "{\"status\":\"accepted\",\"message\":\"Firmware download started\"}");
}

void WebPortal::runFirmwareUpdate(void *parameter)
{
    OtaTaskContext *context = static_cast<OtaTaskContext *>(parameter);
    WebPortal *portal = context->portal;
    WatchdogPause pause;
    bool updateBegun = false;
    String failure;

    portal->setOtaStatus("downloading", "Downloading firmware", 0);
    WiFiClientSecure tlsClient;
    configureOtaTls(tlsClient);
    HTTPClient http;
    http.setTimeout(10000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

    do
    {
        if (portal->mqttManager) portal->mqttManager->disconnect();
        failure = httpsBlocker();
        if (!failure.isEmpty()) break;
        if (!http.begin(tlsClient, context->url))
        {
            failure = "TLS initialization failed";
            break;
        }
        http.addHeader("User-Agent", "expLORA-Gateway/" +
                                     String(FIRMWARE_VERSION));
        const int httpCode = http.GET();
        if (httpCode != HTTP_CODE_OK)
        {
            failure = "Firmware download failed; redirects are not allowed";
            break;
        }

        const int contentLength = http.getSize();
        const esp_partition_t *ota =
            esp_ota_get_next_update_partition(nullptr);
        if (ota == nullptr)
        {
            failure = "No OTA partition is available";
            break;
        }
        if (contentLength <= static_cast<int>(sizeof(esp_image_header_t)) ||
            static_cast<size_t>(contentLength) > ota->size)
        {
            failure = "Invalid firmware size or OTA slot exceeded (Content-Length required)";
            break;
        }

        const size_t beginSize = contentLength > 0
            ? static_cast<size_t>(contentLength)
            : static_cast<size_t>(UPDATE_SIZE_UNKNOWN);
        if (!Update.begin(beginSize))
        {
            failure = "Could not initialize the OTA partition";
            break;
        }
        updateBegun = true;

        mbedtls_sha256_context shaContext;
        mbedtls_sha256_init(&shaContext);
        if (mbedtls_sha256_starts(&shaContext, 0) != 0)
        {
            mbedtls_sha256_free(&shaContext);
            failure = "Could not initialize firmware hashing";
            break;
        }

        WiFiClient *stream = http.getStreamPtr();
        uint8_t buffer[2048];
        size_t totalWritten = 0;
        int remaining = contentLength;
        const uint32_t downloadStarted = millis();
        uint32_t lastDataAt = downloadStarted;
        uint8_t imageHeader[sizeof(esp_image_header_t)];
        size_t imageHeaderLength = 0;
        size_t received = 0;
        int lastProgress = -1;

        while (failure.isEmpty())
        {
            if (millis() - downloadStarted > OTA_DOWNLOAD_TIMEOUT_MS)
            {
                failure = "Firmware download took over 15 minutes";
                break;
            }
            int available = stream->available();
            if (available > 0)
            {
                available = min(available, min(remaining, static_cast<int>(sizeof(buffer))));
                const int read = stream->readBytes(
                    reinterpret_cast<char *>(buffer), available);
                if (read > 0)
                {
                    if (totalWritten + static_cast<size_t>(read) > ota->size)
                    {
                        failure = "Firmware stream exceeds the OTA slot";
                        break;
                    }
                    mbedtls_sha256_update(&shaContext, buffer, static_cast<size_t>(read));
                    received += static_cast<size_t>(read);
                    size_t offset = 0;
                    if (imageHeaderLength < sizeof(imageHeader))
                    {
                        size_t take = std::min(static_cast<size_t>(read), sizeof(imageHeader) - imageHeaderLength);
                        memcpy(imageHeader + imageHeaderLength, buffer, take);
                        imageHeaderLength += take;
                        offset = take;
                        if (imageHeaderLength == sizeof(imageHeader))
                        {
                            const char *imageError = firmwareImageError(imageHeader, imageHeaderLength);
                            if (imageError) { failure = imageError; break; }
                            if (Update.write(imageHeader, sizeof(imageHeader)) != sizeof(imageHeader))
                            { failure = "Firmware header flash write failed"; break; }
                            totalWritten = sizeof(imageHeader);
                        }
                    }
                    if (imageHeaderLength == sizeof(imageHeader) && static_cast<size_t>(read) > offset)
                    {
                        size_t count = static_cast<size_t>(read) - offset;
                        if (Update.write(buffer + offset, count) != count)
                        { failure = "Firmware flash write failed"; break; }
                        totalWritten += count;
                    }
                    lastDataAt = millis();
                    if (remaining > 0)
                    {
                        remaining -= read;
                    }
                    if (contentLength > 0)
                    {
                        const int progress = static_cast<int>(
                            (received * 100ULL) /
                            static_cast<size_t>(contentLength));
                        if (progress >= lastProgress + 5)
                        {
                            lastProgress = progress;
                            portal->setOtaStatus(
                                "downloading", "Downloading firmware",
                                progress);
                        }
                    }
                }
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            if (contentLength > 0 && remaining <= 0)
            {
                break;
            }
            if (!http.connected() && stream->available() == 0)
            {
                break;
            }
            if (millis() - lastDataAt > OTA_STALL_TIMEOUT_MS)
            {
                failure = "Firmware download timed out";
                break;
            }
        }

        uint8_t actualDigest[32];
        mbedtls_sha256_finish(&shaContext, actualDigest);
        mbedtls_sha256_free(&shaContext);

        if (!failure.isEmpty())
        {
            break;
        }
        if (totalWritten == 0 ||
            (contentLength > 0 &&
             totalWritten != static_cast<size_t>(contentLength)))
        {
            failure = "Firmware transfer ended before the complete image arrived";
            break;
        }

        const String actualSha256 = digestToHex(actualDigest);
        if (actualSha256 != context->expectedSha256)
        {
            failure = "Firmware SHA-256 digest does not match metadata";
            break;
        }

        portal->setOtaStatus("installing",
                            "Firmware digest verified; installing image", 100);
        if (!Update.end(true))
        {
            failure = "Firmware image validation failed";
            break;
        }
        updateBegun = false;
    } while (false);

    http.end();
    if (!failure.isEmpty())
    {
        if (updateBegun)
        {
            Update.abort();
        }
        portal->logger.error("URL OTA failed: " + failure);
        portal->logger.info(
            "URL OTA loop minimum free stack: " +
            String(uxTaskGetStackHighWaterMark(nullptr)) + " bytes");
        portal->finishOta(context, "failed", failure, 0);
        delete context;
        return;
    }

    portal->logger.info(
        "URL OTA loop minimum free stack: " +
        String(uxTaskGetStackHighWaterMark(nullptr)) + " bytes");
    portal->logger.info("URL OTA installed; rebooting");
    portal->setOtaStatus("rebooting",
                         "Firmware installed; gateway is rebooting", 100);
    delete context;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP.restart();
}

void WebPortal::handleFirmwarePage(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /firmware");
    if (!authorizeRequest(request, false)) return;
    request->send(200, "text/html", HTMLGenerator::generateFirmwarePage());
}

void WebPortal::cleanupFirmwareUpload(AsyncWebServerRequest *request,
                                      bool disconnected)
{
    FirmwareUploadState *state =
        static_cast<FirmwareUploadState *>(request->_tempObject);
    if (state == nullptr)
    {
        return;
    }
    request->_tempObject = nullptr;

    if (state->shaInitialized)
    {
        mbedtls_sha256_free(&state->shaContext);
        state->shaInitialized = false;
    }
    if (state->updateBegun)
    {
        Update.abort();
        state->updateBegun = false;
    }
    if (state->ownsOta && !state->installed)
    {
        finishOta(request, disconnected ? "failed" : "idle",
                  disconnected ? "Firmware upload was interrupted"
                               : "Firmware upload ended",
                  0);
        state->ownsOta = false;
    }
    else if (state->installed)
    {
        // Update.end() has already selected the verified image. If the client
        // disappears before the completion callback can schedule a restart,
        // still reboot instead of leaving the OTA lock and pending image
        // stranded until a manual power cycle.
        restartAt = millis() + 500;
        restartRequested = true;
    }
    free(state);
}

void WebPortal::handleFirmwareUploadComplete(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /firmware (complete)");
    if (!authorizeRequest(request, true))
    {
        cleanupFirmwareUpload(request, false);
        return;
    }

    FirmwareUploadState *state =
        static_cast<FirmwareUploadState *>(request->_tempObject);
    if (state == nullptr)
    {
        request->send(400, "application/json",
                      "{\"error\":\"No firmware image was received\"}");
        return;
    }
    if (state->error)
    {
        DynamicJsonDocument document(256);
        document["error"] = state->errorMessage;
        String response;
        serializeJson(document, response);
        const int status = state->httpStatus;
        cleanupFirmwareUpload(request, false);
        request->send(status, "application/json", response);
        return;
    }
    if (!state->installed)
    {
        cleanupFirmwareUpload(request, false);
        request->send(400, "application/json",
                      "{\"error\":\"Firmware upload did not complete\"}");
        return;
    }

    const size_t bytes = state->written;
    // Keep the OTA single-flight lock until reboot. The request framework
    // owns _tempObject with malloc/free semantics, so this state is allocated
    // and released the same way.
    request->_tempObject = nullptr;
    free(state);
    request->send(
        200, "application/json",
        "{\"status\":\"installed\",\"message\":\"Firmware installed; device will reboot\",\"bytes\":" +
            String(bytes) + "}");
    restartAt = millis() + 1500;
    restartRequested = true;
}

void WebPortal::handleFirmwareUploadChunk(AsyncWebServerRequest *request,
                                          String filename, size_t index,
                                          uint8_t *data, size_t len, bool final)
{
    (void)filename;
    if (!isRequestAuthorized(request, true))
    {
        return;
    }

    FirmwareUploadState *state =
        static_cast<FirmwareUploadState *>(request->_tempObject);
    if (index == 0)
    {
        if (state != nullptr)
        {
            state->error = true;
            state->httpStatus = 400;
            state->errorMessage = "Only one firmware file per upload";
            if (state->updateBegun) { Update.abort(); state->updateBegun = false; }
            if (state->installed)
            {
                esp_ota_set_boot_partition(esp_ota_get_running_partition());
                state->installed = false;
                restartRequested = false;
            }
            if (state->ownsOta)
            {
                finishOta(request, "failed", state->errorMessage, 0);
                state->ownsOta = false;
            }
            return;
        }
        state = static_cast<FirmwareUploadState *>(
            calloc(1, sizeof(FirmwareUploadState)));
        if (state == nullptr)
        {
            return;
        }
        state->httpStatus = 500;
        state->errorMessage = "Firmware upload failed";
        request->_tempObject = state;
        mbedtls_sha256_init(&state->shaContext);
        request->onDisconnect(
            [this, request]() { cleanupFirmwareUpload(request, true); });

        const String version =
            requestHeader(request, "X-Firmware-Version");
        String expectedSha256 =
            requestHeader(request, "X-Firmware-SHA256");
        expectedSha256.toLowerCase();

        if (!SecurityPolicy::isVersionNewer(version.c_str(),
                                             FIRMWARE_VERSION))
        {
            state->error = true;
            state->httpStatus = 400;
            state->errorMessage =
                "Firmware version must be newer than the running version";
        }
        else if (!SecurityPolicy::isHexString(expectedSha256.c_str(), 64))
        {
            state->error = true;
            state->httpStatus = 400;
            state->errorMessage = "Invalid firmware SHA-256 digest";
        }
        else
        {
            memcpy(state->expectedSha256, expectedSha256.c_str(),
                   sizeof(state->expectedSha256));
        }
        if (state->error)
        {
            return;
        }

        if (!tryBeginOta(request))
        {
            state->error = true;
            state->httpStatus = 409;
            state->errorMessage =
                "Another OTA operation is already in progress";
            return;
        }
        state->ownsOta = true;

        const esp_partition_t *ota =
            esp_ota_get_next_update_partition(nullptr);
        if (ota == nullptr || !Update.begin(UPDATE_SIZE_UNKNOWN))
        {
            state->error = true;
            state->errorMessage = ota == nullptr
                ? "No OTA partition is available"
                : "Could not initialize the OTA partition";
        }
        else
        {
            state->updateBegun = true;
            state->shaInitialized =
                mbedtls_sha256_starts(&state->shaContext, 0) == 0;
            if (!state->shaInitialized)
            {
                state->error = true;
                state->errorMessage =
                    "Could not initialize firmware hashing";
            }
        }

        if (state->error)
        {
            if (state->updateBegun)
            {
                Update.abort();
                state->updateBegun = false;
            }
            finishOta(request, "failed", state->errorMessage, 0);
            state->ownsOta = false;
            return;
        }
        setOtaStatus("uploading", "Receiving firmware upload", 0);
    }

    if (state == nullptr || state->error)
    {
        return;
    }

    auto failUpload = [this, request, state](int status,
                                             const char *message) {
        state->error = true;
        state->httpStatus = status;
        state->errorMessage = message;
        if (state->updateBegun)
        {
            Update.abort();
            state->updateBegun = false;
        }
        if (state->ownsOta)
        {
            finishOta(request, "failed", message, 0);
            state->ownsOta = false;
        }
    };

    if (index != state->written)
    {
        failUpload(400, "Firmware upload chunks arrived out of order");
        return;
    }

    const esp_partition_t *ota = esp_ota_get_next_update_partition(nullptr);
    if (ota == nullptr || state->written + len > ota->size)
    {
        failUpload(413, "Firmware exceeds the OTA slot");
        return;
    }
    if (len > 0)
    {
        mbedtls_sha256_update(&state->shaContext, data, len);
        state->written += len;
        size_t offset = 0;
        if (state->headerLength < sizeof(state->header))
        {
            size_t take = std::min(len, sizeof(state->header) - state->headerLength);
            memcpy(state->header + state->headerLength, data, take);
            state->headerLength += take;
            offset = take;
            if (state->headerLength == sizeof(state->header))
            {
                const char *imageError = firmwareImageError(state->header, state->headerLength);
                if (imageError) { failUpload(400, imageError); return; }
                if (Update.write(state->header, sizeof(state->header)) != sizeof(state->header))
                { failUpload(500, "Firmware header flash write failed"); return; }
                state->flashed = sizeof(state->header);
            }
        }
        if (state->headerLength == sizeof(state->header) && len > offset)
        {
            if (Update.write(data + offset, len - offset) != len - offset)
            { failUpload(500, "Firmware flash write failed"); return; }
            state->flashed += len - offset;
        }
        const int progress = static_cast<int>(
            (state->written * 100ULL) / ota->size);
        setOtaStatus("uploading", "Receiving firmware upload",
                     min(progress, 99));
    }

    if (!final)
    {
        return;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&state->shaContext, digest);
    mbedtls_sha256_free(&state->shaContext);
    state->shaInitialized = false;
    if (state->written == 0 || state->headerLength != sizeof(state->header) || state->flashed != state->written)
    {
        failUpload(400, "Firmware image is empty");
        return;
    }

    const String actualSha256 = digestToHex(digest);
    if (actualSha256 != state->expectedSha256)
    {
        failUpload(400, "Firmware SHA-256 digest does not match metadata");
        return;
    }

    setOtaStatus("installing",
                 "Firmware digest verified; installing image", 100);
    if (!Update.end(true))
    {
        failUpload(500, "Firmware image validation failed");
        return;
    }
    state->updateBegun = false;
    state->installed = true;
    setOtaStatus("rebooting",
                 "Firmware installed; gateway is rebooting", 100);
}
