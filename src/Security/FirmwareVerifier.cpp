#include "FirmwareVerifier.h"

#include "SecurityPolicy.h"
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

namespace
{
// Only the public verification key belongs in firmware. Its private P-256
// counterpart is held by release infrastructure outside this repository.
static const char FIRMWARE_PUBLIC_KEY[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEaAp0uMbANessFCMfgOiYwk1xHLwU\n"
    "8qmkxW8z4w2B7hPwVWK0KnqSM2DrISpbtEwQXRLOlWli2LHlg9NaGXDMjw==\n"
    "-----END PUBLIC KEY-----\n";
}

namespace FirmwareVerifier
{
String canonicalMetadata(const String &version, const String &sha256)
{
    return "expLORA-Gateway-Lite\nversion=" + version +
           "\nsha256=" + sha256 + "\n";
}

String digestToHex(const uint8_t digest[32])
{
    static const char hex[] = "0123456789abcdef";
    char output[65];
    for (size_t i = 0; i < 32; ++i)
    {
        output[i * 2] = hex[(digest[i] >> 4) & 0x0f];
        output[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    output[64] = '\0';
    return String(output);
}

bool verifyMetadataSignature(const String &version,
                             const String &sha256Input,
                             const String &signatureBase64,
                             String &error)
{
    const SecurityPolicy::SemanticVersion parsed =
        SecurityPolicy::parseSemanticVersion(version.c_str());
    String sha256 = sha256Input;
    sha256.toLowerCase();
    if (!parsed.valid)
    {
        error = "Invalid firmware version";
        return false;
    }
    if (!SecurityPolicy::isHexString(sha256.c_str(), 64))
    {
        error = "Invalid SHA-256 digest";
        return false;
    }
    if (!SecurityPolicy::isBase64SignatureShape(signatureBase64.c_str()))
    {
        error = "Invalid firmware signature encoding";
        return false;
    }

    unsigned char signatureDer[80];
    size_t signatureLength = 0;
    int rc = mbedtls_base64_decode(
        signatureDer, sizeof(signatureDer), &signatureLength,
        reinterpret_cast<const unsigned char *>(signatureBase64.c_str()),
        signatureBase64.length());
    if (rc != 0 || signatureLength == 0)
    {
        error = "Invalid firmware signature";
        return false;
    }

    const String metadata = canonicalMetadata(version, sha256);
    uint8_t digest[32];
    rc = mbedtls_sha256_ret(
        reinterpret_cast<const unsigned char *>(metadata.c_str()),
        metadata.length(), digest, 0);
    if (rc != 0)
    {
        error = "Could not hash firmware metadata";
        return false;
    }

    mbedtls_pk_context publicKey;
    mbedtls_pk_init(&publicKey);
    rc = mbedtls_pk_parse_public_key(
        &publicKey,
        reinterpret_cast<const unsigned char *>(FIRMWARE_PUBLIC_KEY),
        sizeof(FIRMWARE_PUBLIC_KEY));
    if (rc == 0 && !mbedtls_pk_can_do(&publicKey, MBEDTLS_PK_ECDSA))
    {
        rc = MBEDTLS_ERR_PK_TYPE_MISMATCH;
    }
    if (rc == 0)
    {
        rc = mbedtls_pk_verify(&publicKey, MBEDTLS_MD_SHA256, digest,
                               sizeof(digest), signatureDer, signatureLength);
    }
    mbedtls_pk_free(&publicKey);

    if (rc != 0)
    {
        error = "Firmware metadata signature is not trusted";
        return false;
    }
    error = "";
    return true;
}
} // namespace FirmwareVerifier
