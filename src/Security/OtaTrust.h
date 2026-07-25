/**
 * TLS trust configuration for the official firmware origin.
 */
#pragma once

#include <WiFiClientSecure.h>

// ESP-IDF's bundled Mozilla root store is enabled by sdkconfig. This permits
// ordinary server certificate renewal without pinning OTA to one CA while
// preserving full certificate and hostname verification.
extern const uint8_t rootca_crt_bundle_start[]
    asm("_binary_x509_crt_bundle_start");

inline void configureOtaTls(WiFiClientSecure &client)
{
    client.setCACertBundle(rootca_crt_bundle_start);
}
