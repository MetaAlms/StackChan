#include <sdkconfig.h>

// Shared by every esp_peer path, independent of M1 observation wrappers.
#if CONFIG_STACKCHAN_WEBRTC_M0 || CONFIG_STACKCHAN_WEBRTC_M1 || CONFIG_STACKCHAN_WEBRTC_M15
#include <mbedtls/ssl.h>
#include <esp_log.h>

extern "C" int __real_mbedtls_ssl_conf_dtls_srtp_protection_profiles(
        mbedtls_ssl_config* conf, const mbedtls_ssl_srtp_profile* profiles);

/**
 * Narrow the DTLS-SRTP protection profiles to the one this stack can produce.
 *
 * esp_peer always installs the AES128_CM_HMAC_SHA1_80 policy, but offers
 * SHA1_32 and NULL profiles as well. The Aliyun gateway prefers SHA1_32 when
 * it is offered, then every media packet fails authentication on the server
 * and is dropped silently: local SRTP/UDP writes succeed, but nothing decodes
 * (findings B35, U1 设备实测)。Until the component selects its srtp policy per
 * negotiated profile, offer only the implemented one.
 */
extern "C" int __wrap_mbedtls_ssl_conf_dtls_srtp_protection_profiles(
        mbedtls_ssl_config* conf, const mbedtls_ssl_srtp_profile* profiles)
{
    static const mbedtls_ssl_srtp_profile kImplemented[] = {
        MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
        MBEDTLS_TLS_SRTP_UNSET,
    };
    int offered = 0;
    while (profiles != nullptr && offered < 8 &&
           profiles[offered] != MBEDTLS_TLS_SRTP_UNSET) {
        ++offered;
    }
    ESP_LOGW("WebRTC-SRTP", "SRTP offer narrowed to SHA1_80 (component offered %d profile(s); "
                  "only the 80-bit policy is implemented)", offered);
    return __real_mbedtls_ssl_conf_dtls_srtp_protection_profiles(conf, kImplemented);
}


#endif
