#pragma once

#include <Arduino.h>

namespace FirmwareVerifier
{
static constexpr const char *KEY_ID = "explora-fw-p256-2026-07";

// The signature covers both the declared version and the exact image digest.
// An older signed image therefore cannot be relabelled as a newer release.
String canonicalMetadata(const String &version, const String &sha256);

bool verifyMetadataSignature(const String &version,
                             const String &sha256,
                             const String &signatureBase64,
                             String &error);

String digestToHex(const uint8_t digest[32]);
} // namespace FirmwareVerifier
