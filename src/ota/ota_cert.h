#pragma once
// Embedded trusted root CA for the OTA HTTPS origin (Phase 5 section 11).
// WiFiClientSecure::setCACert() is used with this string; setInsecure()
// must never be called anywhere in the OTA subsystem, placeholder or not.
//
// This is a placeholder only. It intentionally will not validate against
// any real server: OtaConfig::isPlaceholderServer() (ota_config.h) gates
// every network attempt while OtaConfig::kBaseUrl is still the placeholder
// host, so this string is never actually presented to a live TLS session
// until both are replaced together with the real production values.
//
// Phase 5 section 40: when the real OTA host's CA is later rotated, ship
// the new root through a firmware release before the old one expires —
// never fall back to setInsecure() to work around an expired cert.
constexpr const char* kOtaRootCaPem = R"PEM(
-----BEGIN CERTIFICATE-----
PLACEHOLDER - replace with the real OTA host's root CA certificate
before OtaConfig::kBaseUrl is changed away from its placeholder value.
-----END CERTIFICATE-----
)PEM";
