/**
 * TLS trust configuration backed by ESP-IDF's Mozilla root store.
 */
#pragma once

#include <WiFiClientSecure.h>

// ESP-IDF's bundled Mozilla root store is enabled by sdkconfig. This permits
// ordinary server certificate renewal without pinning OTA to one CA while
// preserving full certificate and hostname verification.
inline void configurePublicCaTls(WiFiClientSecure &client)
{
    client.useBuiltinCACertBundle();
}

inline void configureOtaTls(WiFiClientSecure &client)
{
    configurePublicCaTls(client);
}
