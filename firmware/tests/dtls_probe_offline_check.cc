/**
 * @brief Offline behavioural check for the DTLS short-probe core (U1-0 / D3-1+D3-2).
 *
 * This is the *actual* source under test: the harness includes the same
 * `firmware/main/hal/webrtc/dtls_probe_core.h` that the device firmware compiles,
 * and drives it through the same `Wrap*` entry points that `dtls_short_probe.cc`
 * exposes to GNU ld `--wrap`.
 *
 * The `__real_*` stubs below are declared in that header with the real Mbed TLS
 * prototypes, and the compile-time assertions take their address from the local
 * Mbed TLS headers. A wrong bare signature (`int` for the void
 * `mbedtls_ssl_conf_transport`) or a hardcoded transport number therefore cannot
 * compile, instead of silently mis-classifying HTTPS as DTLS.
 *
 * Scope limits, stated rather than implied:
 *   - This is a C++ compile/run check. It is NOT a single-process controller
 *     check and does not substitute for one.
 *   - It does not exercise the device `--wrap` plumbing, FreeRTOS, the socket
 *     wrappers, or any real handshake; those are reported separately.
 */

#include "dtls_probe_core.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

using dtls_probe::Identity;

// ---------------------------------------------------------------------------
// Compile-time proof that the declarations match the local Mbed TLS headers.
// ---------------------------------------------------------------------------

static_assert(MBEDTLS_SSL_TRANSPORT_STREAM == 0,
              "local Mbed TLS changed STREAM: re-read ssl.h before trusting 0/1");
static_assert(MBEDTLS_SSL_TRANSPORT_DATAGRAM == 1,
              "local Mbed TLS changed DATAGRAM: re-read ssl.h before trusting 0/1");
static_assert(dtls_probe::kTransportStream == MBEDTLS_SSL_TRANSPORT_STREAM,
              "core transport constant must come from the public macro");
static_assert(dtls_probe::kTransportDatagram == MBEDTLS_SSL_TRANSPORT_DATAGRAM,
              "core transport constant must come from the public macro");
static_assert(MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80 == 1,
              "profile 1 used by these checks must be the real IANA value");
static_assert(MBEDTLS_TLS_SRTP_UNSET == 0,
              "UNSET must be the real Mbed TLS sentinel");

// F3: each `__real_*` declaration is compared **directly with the actual public
// Mbed TLS API**, not with a hand-written function type. If upstream changes any
// of these six signatures, this translation unit stops compiling - the
// declaration in dtls_probe_core.h and the real header must agree by
// construction, so a wrong `int` return for the void conf_transport cannot
// survive. (`__real_*` carry C language linkage, so their address type is the
// plain function-pointer type of the real symbol.)
static_assert(std::is_same<decltype(&__real_mbedtls_ssl_conf_transport),
                           decltype(&mbedtls_ssl_conf_transport)>::value,
              "declaration must match the real mbedtls_ssl_conf_transport (ssl.h:2143)");
static_assert(std::is_same<decltype(&__real_mbedtls_ssl_config_free),
                           decltype(&mbedtls_ssl_config_free)>::value,
              "declaration must match the real mbedtls_ssl_config_free (ssl.h:5741)");
static_assert(std::is_same<decltype(&__real_mbedtls_ssl_free),
                           decltype(&mbedtls_ssl_free)>::value,
              "declaration must match the real mbedtls_ssl_free (ssl.h:5569)");
static_assert(std::is_same<decltype(&__real_mbedtls_ssl_setup),
                           decltype(&mbedtls_ssl_setup)>::value,
              "declaration must match the real mbedtls_ssl_setup");
static_assert(std::is_same<decltype(&__real_mbedtls_ssl_config_defaults),
                           decltype(&mbedtls_ssl_config_defaults)>::value,
              "declaration must match the real mbedtls_ssl_config_defaults");
static_assert(std::is_same<decltype(&__real_mbedtls_ssl_handshake),
                           decltype(&mbedtls_ssl_handshake)>::value,
              "declaration must match the real mbedtls_ssl_handshake");

// ---------------------------------------------------------------------------
// Stubs. These stand in for the real Mbed TLS calls that --wrap redirects on the
// device. They deliberately report whether they were reached, so "the wrapper
// exists" can never be mistaken for "the wrapper was called".
// ---------------------------------------------------------------------------

namespace {

struct Stub {
    // Controlled outcomes.
    int handshake_rc = 0;
    int handshake_errno = 0;
    /** When false the real call leaves the incoming errno untouched. */
    bool handshake_sets_errno = true;
    int conf_transport_errno = 0;
    int config_defaults_rc = 0;
    int config_defaults_errno = 0;
    int setup_rc = 0;
    int setup_errno = 0;
    int config_free_errno = 0;
    int ssl_free_errno = 0;
    bool handshake_over = false;
    int srtp_profile = MBEDTLS_TLS_SRTP_UNSET;
    const char* ciphersuite = "TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256";

    // F1 hooks: reset the observation window from inside an observation point.
    // These only call Arm(); they never touch real SSL/config state.
    bool arm_inside_real = false;
    bool arm_inside_over_getter = false;
    // F2 hook: a getter that clobbers errno after the real call has returned.
    bool getter_pollutes_errno = false;

    // Reached / observed counters.
    int conf_transport_reached = 0;
    int config_defaults_reached = 0;
    int setup_reached = 0;
    int config_free_reached = 0;
    int ssl_free_reached = 0;
    int handshake_reached = 0;
    int over_getter_calls = 0;
    int cipher_getter_calls = 0;
    int profile_getter_calls = 0;
    /** The errno the real handshake actually received at its entry (F2). */
    int handshake_entry_errno_seen = -1;
};

Stub g_stub;

void ResetStub()
{
    g_stub = Stub();
}

bool GetHandshakeOver(mbedtls_ssl_context*)
{
    ++g_stub.over_getter_calls;
    if (g_stub.arm_inside_over_getter) {
        dtls_probe::Arm();
    }
    if (g_stub.getter_pollutes_errno) {
        errno = EIO;                       // observer noise, must not escape
    }
    return g_stub.handshake_over;
}

const char* GetCiphersuite(mbedtls_ssl_context*)
{
    ++g_stub.cipher_getter_calls;
    if (g_stub.getter_pollutes_errno) {
        errno = EIO;                       // observer noise, must not escape
    }
    return g_stub.ciphersuite;
}

int GetSrtpProfile(mbedtls_ssl_context*)
{
    ++g_stub.profile_getter_calls;
    if (g_stub.getter_pollutes_errno) {
        errno = EIO;                       // observer noise, must not escape
    }
    return g_stub.srtp_profile;
}

const dtls_probe::HandshakeGetters kGetters = {&GetHandshakeOver, &GetCiphersuite,
                                               &GetSrtpProfile};

int64_t g_fake_now_us = 0;

int64_t NowUs()
{
    g_fake_now_us += 5000;   // deterministic 5 ms per call
    return g_fake_now_us;
}

/** F2: a timing function that deliberately clobbers errno while measuring. */
int64_t NowUsPolluting()
{
    g_fake_now_us += 5000;
    errno = EINTR;
    return g_fake_now_us;
}

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what, const char* file, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s\n", file, line, what);
    }
}

#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        const long long va_ = (long long)(a);                                \
        const long long vb_ = (long long)(b);                                \
        ++g_checks;                                                          \
        if (va_ != vb_) {                                                    \
            ++g_failures;                                                    \
            std::printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__,  \
                        __LINE__, #a, #b, va_, vb_);                         \
        }                                                                    \
    } while (0)

// Distinct opaque objects; the core only ever compares their addresses.
mbedtls_ssl_config g_conf_pool[6];
mbedtls_ssl_context g_ctx_pool[6];

mbedtls_ssl_config* Conf(int i) { return &g_conf_pool[i]; }
mbedtls_ssl_context* Ctx(int i) { return &g_ctx_pool[i]; }

/** Register a configuration and bind a context the way esp_peer does. */
void SetupPair(int ci, int xi, int transport)
{
    dtls_probe::WrapConfigDefaults(Conf(ci), MBEDTLS_SSL_IS_CLIENT, transport,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
    dtls_probe::WrapSetup(Ctx(xi), Conf(ci));
}

const char* Name(Identity id) { return dtls_probe::IdentityName(id); }

}  // namespace

extern "C" {

void __real_mbedtls_ssl_conf_transport(mbedtls_ssl_config* conf, int transport)
{
    (void)conf;
    (void)transport;
    ++g_stub.conf_transport_reached;
    errno = g_stub.conf_transport_errno;
}

int __real_mbedtls_ssl_config_defaults(mbedtls_ssl_config* conf, int endpoint,
                                       int transport, int preset)
{
    (void)conf;
    (void)endpoint;
    (void)transport;
    (void)preset;
    ++g_stub.config_defaults_reached;
    errno = g_stub.config_defaults_errno;
    return g_stub.config_defaults_rc;
}

int __real_mbedtls_ssl_setup(mbedtls_ssl_context* ssl, const mbedtls_ssl_config* conf)
{
    (void)ssl;
    (void)conf;
    ++g_stub.setup_reached;
    errno = g_stub.setup_errno;
    return g_stub.setup_rc;
}

void __real_mbedtls_ssl_config_free(mbedtls_ssl_config* conf)
{
    (void)conf;
    ++g_stub.config_free_reached;
    errno = g_stub.config_free_errno;
}

void __real_mbedtls_ssl_free(mbedtls_ssl_context* ssl)
{
    (void)ssl;
    ++g_stub.ssl_free_reached;
    errno = g_stub.ssl_free_errno;
}

int __real_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl)
{
    (void)ssl;
    ++g_stub.handshake_reached;
    // F2: record what the wrapper actually handed us at entry, before touching
    // errno ourselves.
    g_stub.handshake_entry_errno_seen = errno;
    if (g_stub.arm_inside_real) {
        dtls_probe::Arm();     // F1 hook: resets the observation window only
    }
    if (g_stub.handshake_sets_errno) {
        errno = g_stub.handshake_errno;
    }
    return g_stub.handshake_rc;
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

/** 1. Return values and errno survive every wrapper untouched. */
static void ScenarioReturnAndErrno()
{
    std::printf("[1] return value and errno passthrough\n");
    ResetStub();
    dtls_probe::Arm();

    // void wrapper whose real call leaves a distinctive errno.
    g_stub.conf_transport_errno = EAGAIN;
    errno = 0;
    dtls_probe::WrapConfTransport(Conf(0), MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    CHECK_EQ(g_stub.conf_transport_reached, 1);
    CHECK_EQ(errno, EAGAIN);

    // The report keeps both sides of the discipline as evidence.
    dtls_probe::Report r = dtls_probe::Snapshot();
    CHECK_EQ(r.errno_entry_last, 0);
    CHECK_EQ(r.errno_real_last, EAGAIN);

    // Non-zero return must be forwarded exactly, with its errno.
    g_stub.config_defaults_rc = -0x7100;   // arbitrary real error code
    g_stub.config_defaults_errno = EINVAL;
    errno = 0;
    CHECK_EQ(dtls_probe::WrapConfigDefaults(Conf(1), MBEDTLS_SSL_IS_CLIENT,
                                            MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                            MBEDTLS_SSL_PRESET_DEFAULT),
             -0x7100);
    CHECK_EQ(errno, EINVAL);

    // Zero return must stay zero, and a successful setup binds the context.
    g_stub.setup_rc = 0;
    g_stub.setup_errno = 0;
    errno = EAGAIN;
    CHECK_EQ(dtls_probe::WrapSetup(Ctx(0), Conf(0)), 0);
    CHECK_EQ(errno, 0);
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(0), nullptr, nullptr), Identity::Datagram);

    // A configuration whose config_defaults failed was never registered, so a
    // context set up with it is an unknown transport - never DATAGRAM.
    CHECK_EQ(dtls_probe::WrapSetup(Ctx(1), Conf(1)), 0);
    CHECK_EQ(dtls_probe::Classify(Ctx(1), Conf(1), nullptr, nullptr),
             Identity::UnknownTransport);

    // A failed setup must not bind the context at all.
    g_stub.setup_rc = -0x7780;
    g_stub.setup_errno = ENOMEM;
    errno = 0;
    CHECK_EQ(dtls_probe::WrapSetup(Ctx(2), Conf(0)), -0x7780);
    CHECK_EQ(errno, ENOMEM);
    CHECK_EQ(dtls_probe::Classify(Ctx(2), Conf(0), nullptr, nullptr),
             Identity::UnknownContext);

    // void release wrappers also keep the errno their real call produced.
    g_stub.ssl_free_errno = EBADF;
    errno = 0;
    dtls_probe::WrapSslFree(Ctx(0));
    CHECK_EQ(g_stub.ssl_free_reached, 1);
    CHECK_EQ(errno, EBADF);

    g_stub.config_free_errno = EIO;
    errno = 0;
    dtls_probe::WrapConfigFree(Conf(1));
    CHECK_EQ(g_stub.config_free_reached, 1);
    CHECK_EQ(errno, EIO);

    r = dtls_probe::Snapshot();
    CHECK_EQ(r.conf_transport_calls, 1);
    CHECK_EQ(r.config_defaults_calls, 1);
    CHECK_EQ(r.setup_calls, 3);
    CHECK_EQ(r.config_free_calls, 1);
    CHECK_EQ(r.ssl_free_calls, 1);
}

/** 2. A successful STREAM (HTTPS/TLS) handshake is excluded from DTLS. */
static void ScenarioStreamExcluded()
{
    std::printf("[2] STREAM success is excluded, profile not read\n");
    ResetStub();
    dtls_probe::Arm();

    SetupPair(0, 0, MBEDTLS_SSL_TRANSPORT_STREAM);
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(0), nullptr, nullptr), Identity::Stream);
    CHECK(std::strcmp(Name(Identity::Stream), "stream") == 0);

    g_stub.handshake_rc = 0;
    g_stub.handshake_over = true;
    g_stub.srtp_profile = MBEDTLS_TLS_SRTP_UNSET;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(0), Conf(0), kGetters, &NowUs), 0);

    const dtls_probe::Report r = dtls_probe::Snapshot();
    CHECK_EQ(r.stream_hs_excluded, 1);
    CHECK_EQ(r.dtls_hs_calls, 0);
    CHECK_EQ(r.hs_ok, 0);
    CHECK_EQ(r.profile_reads, 0);
    CHECK(!r.have_profile);
    // The old defect reported exactly this call as "dtls=1 ... profile=0 UNSET".
    CHECK_EQ(g_stub.profile_getter_calls, 0);
    CHECK_EQ(g_stub.over_getter_calls, 0);
}

/** 3. A completed DATAGRAM handshake yields the real chosen SRTP profile. */
static void ScenarioDatagramProfileRead()
{
    std::printf("[3] DATAGRAM success reads the negotiated profile (1)\n");
    ResetStub();
    dtls_probe::Arm();

    SetupPair(0, 0, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    int role = -1;
    int transport = -1;
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(0), &role, &transport), Identity::Datagram);
    CHECK_EQ(transport, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    CHECK_EQ(role, MBEDTLS_SSL_IS_CLIENT);
    CHECK(std::strcmp(Name(Identity::Datagram), "datagram") == 0);

    g_stub.handshake_rc = 0;
    g_stub.handshake_over = true;
    g_stub.srtp_profile = MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80;   // == 1
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(0), Conf(0), kGetters, &NowUs), 0);

    const dtls_probe::Report r = dtls_probe::Snapshot();
    CHECK_EQ(r.dtls_hs_calls, 1);
    CHECK_EQ(r.hs_ok, 1);
    CHECK_EQ(r.stream_hs_excluded, 0);
    CHECK_EQ(r.profile_reads, 1);
    CHECK_EQ(r.profile_skipped, 0);
    CHECK(r.have_profile);
    CHECK_EQ(r.profile, 1);
    CHECK(!r.profile_unset);
    CHECK(r.have_role);
    CHECK_EQ(r.role, MBEDTLS_SSL_IS_CLIENT);
    CHECK(r.have_cipher);
    CHECK_EQ(g_stub.profile_getter_calls, 1);
}

/** 4. An incomplete DATAGRAM handshake must not read or report a profile. */
static void ScenarioDatagramIncomplete()
{
    std::printf("[4] incomplete DATAGRAM handshake does not read the profile\n");
    ResetStub();
    dtls_probe::Arm();

    SetupPair(0, 0, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    g_stub.handshake_over = false;
    g_stub.handshake_rc = -0x7780;   // a real timeout-class failure
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(0), Conf(0), kGetters, &NowUs), -0x7780);

    dtls_probe::Report r = dtls_probe::Snapshot();
    CHECK_EQ(r.dtls_hs_calls, 1);
    CHECK_EQ(r.hs_fail, 1);
    CHECK_EQ(r.profile_reads, 0);
    CHECK(!r.have_profile);
    CHECK_EQ(g_stub.profile_getter_calls, 0);
    CHECK_EQ(r.profile_skipped, 1);

    // Success but not yet complete is still not a read.
    dtls_probe::Arm();
    SetupPair(1, 1, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    g_stub.handshake_over = false;
    g_stub.handshake_rc = 0;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(1), Conf(1), kGetters, &NowUs), 0);
    r = dtls_probe::Snapshot();
    CHECK_EQ(r.profile_reads, 0);
    CHECK(!r.have_profile);
    CHECK_EQ(g_stub.profile_getter_calls, 0);
}

/** 5. Unknown identity stays unknown: a config alone never proves a setup. */
static void ScenarioUnknownSetup()
{
    std::printf("[5] unknown setup is reported, never assumed to be DTLS\n");
    ResetStub();
    dtls_probe::Arm();

    // (a) A context that no successful setup registered.
    g_stub.handshake_over = true;
    g_stub.handshake_rc = 0;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(0), Conf(0), kGetters, &NowUs), 0);
    dtls_probe::Report r = dtls_probe::Snapshot();
    CHECK_EQ(r.unknown_ctx, 1);
    CHECK_EQ(r.dtls_hs_calls, 0);
    CHECK_EQ(r.profile_reads, 0);
    CHECK_EQ(g_stub.profile_getter_calls, 0);

    // (b) The exact b45aaa7 fallback: the configuration IS a known DATAGRAM
    // configuration, but this context was never set up with it.
    dtls_probe::Arm();
    SetupPair(2, 2, MBEDTLS_SSL_TRANSPORT_DATAGRAM);   // binds ctx2, not ctx0
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(2), nullptr, nullptr),
             Identity::UnknownContext);
    g_stub.handshake_over = true;
    g_stub.handshake_rc = 0;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(0), Conf(2), kGetters, &NowUs), 0);
    r = dtls_probe::Snapshot();
    CHECK_EQ(r.unknown_ctx, 1);
    CHECK_EQ(r.dtls_hs_calls, 0);
    CHECK_EQ(g_stub.profile_getter_calls, 0);

    // (c) A context bound to a configuration whose transport was never observed.
    dtls_probe::Arm();
    CHECK_EQ(dtls_probe::WrapSetup(Ctx(3), Conf(3)), 0);   // no defaults/transport call
    CHECK_EQ(dtls_probe::Classify(Ctx(3), Conf(3), nullptr, nullptr),
             Identity::UnknownTransport);
    g_stub.handshake_over = true;
    g_stub.handshake_rc = 0;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(3), Conf(3), kGetters, &NowUs), 0);
    r = dtls_probe::Snapshot();
    CHECK_EQ(r.unknown_transport, 1);
    CHECK_EQ(r.dtls_hs_calls, 0);
    CHECK_EQ(g_stub.profile_getter_calls, 0);

    // (d) A context whose live config no longer matches its registration.
    dtls_probe::Arm();
    SetupPair(4, 4, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    CHECK_EQ(dtls_probe::Classify(Ctx(4), Conf(5), nullptr, nullptr),
             Identity::UnknownTransport);

    // (e) A registration from an earlier generation never classifies a call.
    dtls_probe::Arm();
    SetupPair(0, 0, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    {
        dtls_probe::State& st = dtls_probe::Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        for (size_t i = 0; i < dtls_probe::kCtxTable; ++i) {
            if (st.ctxs[i].ctx == Ctx(0)) {
                st.ctxs[i].generation = st.generation - 1;   // simulate a stale entry
            }
        }
    }
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(0), nullptr, nullptr),
             Identity::StaleGeneration);
    g_stub.handshake_over = true;
    g_stub.handshake_rc = 0;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(0), Conf(0), kGetters, &NowUs), 0);
    r = dtls_probe::Snapshot();
    CHECK_EQ(r.stale_ctx, 1);
    CHECK_EQ(r.dtls_hs_calls, 0);
    CHECK_EQ(g_stub.profile_getter_calls, 0);
}

/** 6. conf_transport updates an existing configuration instead of early-returning. */
static void ScenarioTransportSwitch()
{
    std::printf("[6] conf_transport switches STREAM -> DATAGRAM\n");
    ResetStub();
    dtls_probe::Arm();

    // esp_peer order: config_defaults() then conf_transport() on the same config.
    dtls_probe::WrapConfigDefaults(Conf(0), MBEDTLS_SSL_IS_CLIENT,
                                   MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
    CHECK_EQ(dtls_probe::Classify(nullptr, nullptr, nullptr, nullptr),
             Identity::UnknownContext);
    dtls_probe::WrapSetup(Ctx(0), Conf(0));
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(0), nullptr, nullptr), Identity::Stream);

    dtls_probe::WrapConfTransport(Conf(0), MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    dtls_probe::Report r = dtls_probe::Snapshot();
    CHECK_EQ(r.transport_updates, 1);
    CHECK_EQ(r.stream_setups, 1);
    CHECK_EQ(r.setups, 1);   // updated in place, not registered twice
    // The context bound before the switch keeps its own snapshot, which is the
    // recorded setup-time identity, not a live guess.
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(0), nullptr, nullptr), Identity::Stream);

    // A context bound after the switch is a DATAGRAM identity and may read profile.
    dtls_probe::WrapSetup(Ctx(1), Conf(0));
    CHECK_EQ(dtls_probe::Classify(Ctx(1), Conf(0), nullptr, nullptr), Identity::Datagram);
    g_stub.handshake_over = true;
    g_stub.handshake_rc = 0;
    g_stub.srtp_profile = MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80;
    dtls_probe::WrapHandshake(Ctx(1), Conf(0), kGetters, &NowUs);
    r = dtls_probe::Snapshot();
    CHECK_EQ(r.dtls_hs_calls, 1);
    CHECK_EQ(r.profile_reads, 1);
    CHECK_EQ(r.profile, 1);

    // Switching back is recorded too (transport_updates counts every change).
    dtls_probe::WrapConfTransport(Conf(0), MBEDTLS_SSL_TRANSPORT_STREAM);
    CHECK_EQ(dtls_probe::Snapshot().transport_updates, 2);

    // config_defaults on a re-initialised config updates the role as well.
    dtls_probe::Arm();
    ResetStub();
    dtls_probe::WrapConfigDefaults(Conf(0), MBEDTLS_SSL_IS_CLIENT,
                                   MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
    dtls_probe::WrapConfigDefaults(Conf(0), MBEDTLS_SSL_IS_SERVER,
                                   MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
    dtls_probe::WrapSetup(Ctx(0), Conf(0));
    int role = -1;
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(0), &role, nullptr), Identity::Datagram);
    CHECK_EQ(role, MBEDTLS_SSL_IS_SERVER);
    CHECK_EQ(dtls_probe::Snapshot().setups, 1);
}

/** 7. Identity ends on free and on Arm, including the reused-context lifecycle. */
static void ScenarioIdentityEnds()
{
    std::printf("[7] identity ends at free / Arm, survives ctx reuse correctly\n");
    ResetStub();
    dtls_probe::Arm();

    // esp_peer dtls_srtp_reset_session(): ssl_free -> init -> setup on the SAME
    // context pointer. The re-bind must replace the old identity.
    SetupPair(0, 0, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    g_stub.handshake_over = true;
    g_stub.handshake_rc = 0;
    dtls_probe::WrapHandshake(Ctx(0), Conf(0), kGetters, &NowUs);
    CHECK_EQ(dtls_probe::Snapshot().dtls_hs_calls, 1);

    dtls_probe::WrapSslFree(Ctx(0));
    CHECK_EQ(g_stub.ssl_free_reached, 1);
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(0), nullptr, nullptr),
             Identity::UnknownContext);
    // A handshake racing after the release is unknown, never DATAGRAM.
    dtls_probe::WrapHandshake(Ctx(0), Conf(0), kGetters, &NowUs);
    CHECK_EQ(dtls_probe::Snapshot().dtls_hs_calls, 1);
    CHECK_EQ(dtls_probe::Snapshot().unknown_ctx, 1);

    // Re-bind the same pointer: identity is restored from the new setup.
    dtls_probe::WrapSetup(Ctx(0), Conf(0));
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(0), nullptr, nullptr), Identity::Datagram);
    dtls_probe::WrapHandshake(Ctx(0), Conf(0), kGetters, &NowUs);
    CHECK_EQ(dtls_probe::Snapshot().dtls_hs_calls, 2);

    // config_free ends the setup identity: a later context bound to it is unknown.
    dtls_probe::WrapConfigFree(Conf(0));
    CHECK_EQ(g_stub.config_free_reached, 1);
    CHECK_EQ(dtls_probe::WrapSetup(Ctx(1), Conf(0)), 0);
    CHECK_EQ(dtls_probe::Classify(Ctx(1), Conf(0), nullptr, nullptr),
             Identity::UnknownTransport);

    // Arm(): new generation, no old identity, counters reset, generation grows.
    const dtls_probe::Report before = dtls_probe::Snapshot();
    dtls_probe::Arm();
    const dtls_probe::Report after = dtls_probe::Snapshot();
    CHECK_EQ(after.generation, before.generation + 1);
    CHECK_EQ(after.dtls_hs_calls, 0);
    CHECK_EQ(after.setups, 0);
    CHECK_EQ(after.ctx_bound, 0);
    CHECK_EQ(dtls_probe::Classify(Ctx(0), Conf(0), nullptr, nullptr),
             Identity::UnknownContext);
}

/** 8. The fixed tables report overflow instead of silently dropping identity. */
static void ScenarioTableOverflow()
{
    std::printf("[8] fixed tables report overflow explicitly\n");
    ResetStub();
    dtls_probe::Arm();

    for (int i = 0; i < 6; ++i) {
        dtls_probe::WrapConfigDefaults(Conf(i), MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    }
    dtls_probe::Report r = dtls_probe::Snapshot();
    CHECK_EQ(r.setups, 6);
    CHECK_EQ(r.datagram_setups, 6);
    CHECK_EQ(r.setup_overflow, 0);
    CHECK_EQ(r.stream_setups, 0);
    CHECK_EQ(r.unknown_setups, 0);

    // Configure more configurations than the fixed table holds.
    static mbedtls_ssl_config extra[4];
    for (int i = 0; i < 4; ++i) {
        dtls_probe::WrapConfTransport(&extra[i], MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    }
    r = dtls_probe::Snapshot();
    CHECK_EQ(r.setups, dtls_probe::kSetupTable);
    CHECK(r.setup_overflow > 0);

    // And more contexts than the context table holds.
    static mbedtls_ssl_context extra_ctx[dtls_probe::kCtxTable + 4];
    for (size_t i = 0; i < dtls_probe::kCtxTable + 4; ++i) {
        dtls_probe::WrapSetup(&extra_ctx[i], Conf(0));
    }
    r = dtls_probe::Snapshot();
    CHECK(r.ctx_overflow > 0);
}

/**
 * 9. F1: a handshake that outlives an Arm() must not write into the new report.
 *
 * The hooks are deterministic: Arm() is called from inside the real handshake,
 * and from inside the completion getter. They only reset the observation window;
 * they never touch real SSL/config state.
 */
static void ScenarioArmDuringHandshake()
{
    std::printf("[9] F1: stale handshake cannot write into a new Arm report\n");

    // (a) Arm() inside the real call: nothing of this call may appear anywhere.
    ResetStub();
    dtls_probe::Arm();
    SetupPair(0, 0, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    const uint32_t gen_before = dtls_probe::Snapshot().generation;

    g_stub.arm_inside_real = true;
    g_stub.handshake_rc = 0;
    g_stub.handshake_over = true;
    g_stub.srtp_profile = MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80;
    g_stub.handshake_errno = EACCES;
    errno = ENOENT;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(0), Conf(0), kGetters, &NowUs), 0);
    // The real call and its return/errno passthrough are unaffected.
    CHECK_EQ(g_stub.handshake_reached, 1);
    CHECK_EQ(errno, EACCES);

    dtls_probe::Report r = dtls_probe::Snapshot();
    CHECK_EQ(r.generation, gen_before + 1);        // Arm really happened
    CHECK_EQ(r.hs_calls, 0);                       // ...and nothing was recorded
    CHECK_EQ(r.dtls_hs_calls, 0);
    CHECK_EQ(r.hs_ok, 0);
    CHECK_EQ(r.hs_fail, 0);
    CHECK_EQ(r.unknown_ctx, 0);
    CHECK_EQ(r.unknown_transport, 0);
    CHECK_EQ(r.stream_hs_excluded, 0);
    CHECK_EQ(r.profile_reads, 0);
    CHECK_EQ(r.profile_skipped, 0);
    CHECK(!r.have_profile);
    CHECK(!r.handshake_over);
    CHECK(!r.have_cipher);
    CHECK(!r.have_role);
    CHECK_EQ(r.last_identity, -1);
    // The getters are not even consulted once the call is known to be stale.
    CHECK_EQ(g_stub.over_getter_calls, 0);
    CHECK_EQ(g_stub.profile_getter_calls, 0);

    // (b) Arm() inside the completion getter: still no completion/profile and no
    // call counts in the new report, and the profile is never read.
    ResetStub();
    dtls_probe::Arm();
    SetupPair(1, 1, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    const uint32_t gen_before_b = dtls_probe::Snapshot().generation;

    g_stub.arm_inside_over_getter = true;
    g_stub.handshake_rc = 0;
    g_stub.handshake_over = true;
    g_stub.srtp_profile = MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80;
    g_stub.handshake_errno = EACCES;
    errno = ENOENT;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(1), Conf(1), kGetters, &NowUs), 0);
    CHECK_EQ(g_stub.handshake_reached, 1);
    CHECK_EQ(errno, EACCES);
    CHECK_EQ(g_stub.over_getter_calls, 1);         // the hook ran here
    CHECK_EQ(g_stub.profile_getter_calls, 0);      // re-checked before the read

    r = dtls_probe::Snapshot();
    CHECK_EQ(r.generation, gen_before_b + 1);
    CHECK_EQ(r.hs_calls, 0);
    CHECK_EQ(r.dtls_hs_calls, 0);
    CHECK_EQ(r.hs_ok, 0);
    CHECK_EQ(r.profile_reads, 0);
    CHECK_EQ(r.profile_skipped, 0);
    CHECK(!r.have_profile);
    CHECK(!r.handshake_over);
    CHECK(!r.have_cipher);

    // (c) A live call still records normally afterwards: the generation guard
    // rejects only the stale call, not the diagnostic itself.
    ResetStub();
    dtls_probe::Arm();
    SetupPair(2, 2, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    g_stub.handshake_rc = 0;
    g_stub.handshake_over = true;
    g_stub.srtp_profile = MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(2), Conf(2), kGetters, &NowUs), 0);
    r = dtls_probe::Snapshot();
    CHECK_EQ(r.hs_calls, 1);
    CHECK_EQ(r.dtls_hs_calls, 1);
    CHECK_EQ(r.hs_ok, 1);
    CHECK_EQ(r.profile_reads, 1);
    CHECK_EQ(r.profile, 1);
    CHECK(r.handshake_over);
}

/**
 * 10. F2: front-loaded observation must not change the real call's entry errno,
 * and the exit errno must be exactly what the real call produced.
 */
static void ScenarioErrnoPassthrough()
{
    std::printf("[10] F2: timing/getter noise never reaches the real call or caller\n");

    // (a) A polluting timing function, real call leaves errno alone: the real
    // call must still see the caller's entry errno, and the caller must get it
    // back unchanged.
    ResetStub();
    dtls_probe::Arm();
    SetupPair(0, 0, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    g_stub.handshake_rc = 0;
    g_stub.handshake_sets_errno = false;    // real does not overwrite errno
    errno = ENOENT;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(0), Conf(0), kGetters, &NowUsPolluting), 0);
    CHECK_EQ(g_stub.handshake_entry_errno_seen, ENOENT);   // entry passthrough
    CHECK_EQ(errno, ENOENT);                               // exit passthrough

    // (b) Same, but the real call does set errno: the caller sees exactly that.
    ResetStub();
    dtls_probe::Arm();
    SetupPair(1, 1, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    g_stub.handshake_rc = 0;
    g_stub.handshake_sets_errno = true;
    g_stub.handshake_errno = EACCES;
    errno = ENOENT;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(1), Conf(1), kGetters, &NowUsPolluting), 0);
    CHECK_EQ(g_stub.handshake_entry_errno_seen, ENOENT);
    CHECK_EQ(errno, EACCES);
    dtls_probe::Report r = dtls_probe::Snapshot();
    CHECK_EQ(r.errno_entry_last, ENOENT);
    CHECK_EQ(r.errno_real_last, EACCES);

    // (c) Getters that clobber errno after the real call must not change what the
    // caller observes.
    ResetStub();
    dtls_probe::Arm();
    SetupPair(2, 2, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    g_stub.handshake_rc = 0;
    g_stub.handshake_over = true;
    g_stub.srtp_profile = MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80;
    g_stub.getter_pollutes_errno = true;
    g_stub.handshake_sets_errno = true;
    g_stub.handshake_errno = EACCES;
    errno = ENOENT;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(2), Conf(2), kGetters, &NowUsPolluting), 0);
    CHECK_EQ(g_stub.handshake_entry_errno_seen, ENOENT);
    CHECK_EQ(g_stub.profile_getter_calls, 1);   // the getters did run and pollute
    CHECK_EQ(g_stub.over_getter_calls, 1);
    CHECK_EQ(errno, EACCES);                    // ...and still did not escape

    // (d) A failing handshake keeps both sides of the passthrough too.
    ResetStub();
    dtls_probe::Arm();
    SetupPair(3, 3, MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    g_stub.handshake_rc = -0x7780;
    g_stub.handshake_sets_errno = true;
    g_stub.handshake_errno = ETIMEDOUT;
    errno = ENOENT;
    CHECK_EQ(dtls_probe::WrapHandshake(Ctx(3), Conf(3), kGetters, &NowUsPolluting),
             -0x7780);
    CHECK_EQ(g_stub.handshake_entry_errno_seen, ENOENT);
    CHECK_EQ(errno, ETIMEDOUT);
}

int main()
{
    std::printf("DTLS probe core offline check (actual source: dtls_probe_core.h)\n");
    std::printf("local Mbed TLS transport: STREAM=%d DATAGRAM=%d\n",
                (int)MBEDTLS_SSL_TRANSPORT_STREAM, (int)MBEDTLS_SSL_TRANSPORT_DATAGRAM);

    ScenarioReturnAndErrno();
    ScenarioStreamExcluded();
    ScenarioDatagramProfileRead();
    ScenarioDatagramIncomplete();
    ScenarioUnknownSetup();
    ScenarioTransportSwitch();
    ScenarioIdentityEnds();
    ScenarioTableOverflow();
    ScenarioArmDuringHandshake();
    ScenarioErrnoPassthrough();

    std::printf("checks=%d failures=%d -> %s\n", g_checks, g_failures,
                g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
