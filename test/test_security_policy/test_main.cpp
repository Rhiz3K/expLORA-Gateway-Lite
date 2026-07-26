/**
 * Host-native tests for security policy helpers.
 *
 * Run: pio test -e native
 */

#include <unity.h>
#include "../../src/Security/SecurityPolicy.h"

void setUp(void) {}
void tearDown(void) {}

static void test_credential_format_accepts_generated_shape(void)
{
    TEST_ASSERT_TRUE(SecurityPolicy::isAccessCredentialFormat("ABCDEFGHJKLMNPQRST23"));
    TEST_ASSERT_EQUAL_size_t(20, SecurityPolicy::ACCESS_CREDENTIAL_LENGTH);
}

static void test_credential_format_rejects_short_or_ambiguous_values(void)
{
    TEST_ASSERT_FALSE(SecurityPolicy::isAccessCredentialFormat("ABCDEFGHJKLM234"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAccessCredentialFormat("ABCDEFGHJKLMNPQRSTI2"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAccessCredentialFormat("abcdefghijklmnopqrst"));
}

static void test_html_escape_handles_text_and_attribute_metacharacters(void)
{
    TEST_ASSERT_EQUAL_STRING(
        "&lt;tag a=&quot;x&quot;&gt;Tom &amp; &#39;Sue&#39;&lt;/tag&gt;",
        SecurityPolicy::htmlEscape("<tag a=\"x\">Tom & 'Sue'</tag>").c_str());
}

static void test_csv_escape_quotes_fields_and_blocks_formula_prefixes(void)
{
    TEST_ASSERT_EQUAL_STRING(
        "\"sensor, \"\"north\"\"\"",
        SecurityPolicy::csvEscape("sensor, \"north\"").c_str());
    TEST_ASSERT_EQUAL_STRING(
        "'=2+2",
        SecurityPolicy::csvEscape("=2+2").c_str());
}

static void test_secret_update_preserves_replaces_and_clears(void)
{
    TEST_ASSERT_EQUAL_STRING(
        "stored-secret",
        SecurityPolicy::resolveSecret("stored-secret", "", false).c_str());
    TEST_ASSERT_EQUAL_STRING(
        "replacement",
        SecurityPolicy::resolveSecret("stored-secret", "replacement", false).c_str());
    TEST_ASSERT_EQUAL_STRING(
        "",
        SecurityPolicy::resolveSecret("stored-secret", "replacement", true).c_str());
}

static void test_official_firmware_urls_are_allowed(void)
{
    TEST_ASSERT_TRUE(SecurityPolicy::isAllowedFirmwareUrl(
        "https://pajenicko.cz/webflash/explora-gw-lite/firmware.bin"));
    TEST_ASSERT_TRUE(SecurityPolicy::isAllowedFirmwareUrl(
        "https://PAJENICKO.CZ:443/webflash/explora-gw-lite/release.bin?channel=stable"));
}

static void test_manifest_url_has_separate_exact_policy(void)
{
    TEST_ASSERT_TRUE(SecurityPolicy::isAllowedManifestUrl(
        "https://pajenicko.cz/webflash/explora-gw-lite-autoupdate.json"));
    TEST_ASSERT_TRUE(SecurityPolicy::isAllowedManifestUrl(
        "https://PAJENICKO.CZ/webflash/explora-gw-lite-autoupdate.json"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedManifestUrl(
        "https://pajenicko.cz/webflash/explora-gw-lite/firmware.bin"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedManifestUrl(
        "https://pajenicko.cz/webflash/explora-gw-lite-autoupdate.json?channel=dev"));
}

static void test_semantic_version_ordering(void)
{
    TEST_ASSERT_TRUE(SecurityPolicy::isVersionNewer("1.1.14", "1.1.14-dev-secure"));
    TEST_ASSERT_TRUE(SecurityPolicy::isVersionNewer("1.1.15", "1.1.14"));
    TEST_ASSERT_TRUE(SecurityPolicy::isVersionNewer(
        "1.1.14-dev-correctness.10", "1.1.14-dev-correctness.5"));
    TEST_ASSERT_TRUE(SecurityPolicy::isVersionNewer(
        "1.1.14-alpha.1", "1.1.14-alpha"));
    TEST_ASSERT_TRUE(SecurityPolicy::isVersionNewer(
        "1.1.14-beta", "1.1.14-10"));
    TEST_ASSERT_FALSE(SecurityPolicy::isVersionNewer("1.1.13", "1.1.14-dev-secure"));
    TEST_ASSERT_FALSE(SecurityPolicy::isVersionNewer("1.1.14", "1.1.14"));
    TEST_ASSERT_FALSE(SecurityPolicy::isVersionNewer(
        "1.1.14-alpha.5", "1.1.14-alpha.10"));
    TEST_ASSERT_FALSE(SecurityPolicy::isVersionNewer(
        "1.1.14-alpha", "1.1.14-alpha.1"));
    TEST_ASSERT_FALSE(SecurityPolicy::isVersionNewer(
        "1.1.14+build.2", "1.1.14+build.1"));
    TEST_ASSERT_FALSE(SecurityPolicy::isVersionNewer(
        "1.1.14-alpha..1", "1.1.14-alpha"));
    TEST_ASSERT_FALSE(SecurityPolicy::isVersionNewer(
        "1.1.14-alpha.01", "1.1.14-alpha"));
    TEST_ASSERT_FALSE(SecurityPolicy::isVersionNewer(
        "1.01.14", "1.1.14"));
    TEST_ASSERT_FALSE(SecurityPolicy::isVersionNewer("not-a-version", "1.1.14"));
}

static void test_firmware_url_rejects_scheme_host_userinfo_and_port_bypasses(void)
{
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedFirmwareUrl(
        "http://pajenicko.cz/webflash/explora-gw-lite/firmware.bin"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedFirmwareUrl(
        "https://evil.example/webflash/explora-gw-lite/firmware.bin"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedFirmwareUrl(
        "https://pajenicko.cz.evil.example/webflash/explora-gw-lite/firmware.bin"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedFirmwareUrl(
        "https://pajenicko.cz@evil.example/webflash/explora-gw-lite/firmware.bin"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedFirmwareUrl(
        "https://pajenicko.cz:8443/webflash/explora-gw-lite/firmware.bin"));
}

static void test_firmware_url_rejects_deceptive_or_encoded_paths(void)
{
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedFirmwareUrl(
        "https://pajenicko.cz/webflash/explora-gw-lite.bin"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedFirmwareUrl(
        "https://pajenicko.cz/webflash/explora-gw-lite/%2e%2e/evil.bin"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedFirmwareUrl(
        "https://pajenicko.cz/webflash/explora-gw-lite/../evil.bin"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedFirmwareUrl(
        "https://pajenicko.cz/webflash/explora-gw-lite//evil.bin"));
}

static void test_browser_origin_policy_requires_exact_origin(void)
{
    TEST_ASSERT_TRUE(SecurityPolicy::isAllowedBrowserOrigin("", "192.168.4.1"));
    TEST_ASSERT_TRUE(SecurityPolicy::isAllowedBrowserOrigin(
        "http://192.168.4.1", "192.168.4.1"));
    TEST_ASSERT_TRUE(SecurityPolicy::isAllowedBrowserOrigin(
        "http://gateway.local:8080", "GATEWAY.local:8080"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedBrowserOrigin(
        "https://gateway.local", "GATEWAY.local"));
    TEST_ASSERT_TRUE(SecurityPolicy::isAllowedBrowserOrigin(
        "https://gateway.local", "GATEWAY.local:443"));
    TEST_ASSERT_TRUE(SecurityPolicy::isAllowedBrowserOrigin(
        "https://gateway.local:8443", "GATEWAY.local:8443"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedBrowserOrigin(
        "http://evil.example", "192.168.4.1"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedBrowserOrigin(
        "https://gateway.local", "gateway.local:80"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedBrowserOrigin(
        "http://192.168.4.1.evil.example", "192.168.4.1"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedBrowserOrigin(
        "null", "192.168.4.1"));
    TEST_ASSERT_FALSE(SecurityPolicy::isAllowedBrowserOrigin(
        "http://gateway.local:4294967376", "gateway.local:80"));
}

static void test_ap_lifecycle_decisions_cover_late_reconnect(void)
{
    using SecurityPolicy::ApLifecycleAction;
    using SecurityPolicy::ApLifecycleEvent;

    TEST_ASSERT_EQUAL(
        ApLifecycleAction::EnsureRunning,
        SecurityPolicy::decideApLifecycleAction(ApLifecycleEvent::StaDisconnected, false));
    TEST_ASSERT_EQUAL(
        ApLifecycleAction::KeepCurrent,
        SecurityPolicy::decideApLifecycleAction(ApLifecycleEvent::TemporaryWindowExpired, false));
    TEST_ASSERT_EQUAL(
        ApLifecycleAction::ShutDown,
        SecurityPolicy::decideApLifecycleAction(ApLifecycleEvent::TemporaryWindowExpired, true));
    TEST_ASSERT_EQUAL(
        ApLifecycleAction::ShutDown,
        SecurityPolicy::decideApLifecycleAction(ApLifecycleEvent::StaReconnected, true));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_credential_format_accepts_generated_shape);
    RUN_TEST(test_credential_format_rejects_short_or_ambiguous_values);
    RUN_TEST(test_html_escape_handles_text_and_attribute_metacharacters);
    RUN_TEST(test_csv_escape_quotes_fields_and_blocks_formula_prefixes);
    RUN_TEST(test_secret_update_preserves_replaces_and_clears);
    RUN_TEST(test_official_firmware_urls_are_allowed);
    RUN_TEST(test_manifest_url_has_separate_exact_policy);
    RUN_TEST(test_semantic_version_ordering);
    RUN_TEST(test_firmware_url_rejects_scheme_host_userinfo_and_port_bypasses);
    RUN_TEST(test_firmware_url_rejects_deceptive_or_encoded_paths);
    RUN_TEST(test_browser_origin_policy_requires_exact_origin);
    RUN_TEST(test_ap_lifecycle_decisions_cover_late_reconnect);
    return UNITY_END();
}
