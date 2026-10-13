#pragma once

/**
 * @brief Host-compilable core of the short DTLS diagnostic (D3-1 / D3-2 repair).
 *
 * This header is deliberately free of ESP-IDF dependencies so the *same* code
 * that runs on the device can be compiled and executed by the offline check in
 * `firmware/tests/dtls_probe_offline_check.cc`. Nothing here is a copy of the
 * device logic: the device translation unit and the offline check both include
 * this file.
 *
 * Two defects are fixed here relative to b45aaa7:
 *
 *  - D3-1: the transport of a context was classified with bare 0/1 literals in
 *    the wrong order (Mbed TLS `ssl.h` defines STREAM=0, DATAGRAM=1). Every
 *    comparison below goes through `MBEDTLS_SSL_TRANSPORT_STREAM` /
 *    `MBEDTLS_SSL_TRANSPORT_DATAGRAM`, and a context is only ever classified
 *    DATAGRAM when *the same successful setup* registered that exact
 *    (context, configuration) pair. A configuration known on its own is never
 *    enough: an unknown identity stays unknown.
 *
 *  - D3-2: `mbedtls_ssl_conf_transport` returns void, the release function is
 *    `mbedtls_ssl_config_free` (there is no `mbedtls_ssl_conf_free`), and
 *    identity must also end when `mbedtls_ssl_free` runs. Every wrapper below
 *    keeps the real return value, saves the caller's errno before the real call
 *    and restores the errno the real call produced, and records whether it was
 *    actually reached (`*_calls` counters) - a wrapper that has a definition is
 *    not evidence that it is called.
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <mbedtls/ssl.h>

/**
 * Real (unwrapped) entry points.
 *
 * Declared at global scope with C language linkage, because that is exactly what
 * GNU ld `--wrap` renames: `__real_<symbol>` is the original, unmangled symbol.
 *
 * On the device these names come from the linker. The offline check provides
 * stubs with *these* prototypes, so a signature that disagrees with the local
 * Mbed TLS headers cannot compile: `mbedtls_ssl_conf_transport` really returns
 * void, and a stray `int` here would be rejected by the stub definition. Bare
 * numeric transport values are never used as a substitute for the public macros.
 */
extern "C" {
int __real_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl);
void __real_mbedtls_ssl_conf_transport(mbedtls_ssl_config* conf, int transport);
int __real_mbedtls_ssl_setup(mbedtls_ssl_context* ssl, const mbedtls_ssl_config* conf);
int __real_mbedtls_ssl_config_defaults(mbedtls_ssl_config* conf, int endpoint,
                                       int transport, int preset);
void __real_mbedtls_ssl_config_free(mbedtls_ssl_config* conf);
void __real_mbedtls_ssl_free(mbedtls_ssl_context* ssl);
}

namespace dtls_probe {

/** Transport values, taken from the public Mbed TLS macros and nowhere else. */
constexpr int kTransportStream = MBEDTLS_SSL_TRANSPORT_STREAM;       // 0
constexpr int kTransportDatagram = MBEDTLS_SSL_TRANSPORT_DATAGRAM;   // 1
constexpr int kTransportUnknown = -1;
constexpr int kRoleUnknown = -1;

/** Identity of one observed `mbedtls_ssl_handshake()` call. */
enum class Identity : int {
    /** DATAGRAM context registered by the same successful setup: real DTLS. */
    Datagram = 0,
    /** STREAM context registered by the same successful setup: HTTPS/TLS. */
    Stream,
    /** No successful setup registered this context. */
    UnknownContext,
    /** The registration belongs to an earlier Arm() generation. */
    StaleGeneration,
    /** Context registered, but its configuration's transport was never seen. */
    UnknownTransport,
};

inline const char* IdentityName(Identity id)
{
    switch (id) {
        case Identity::Datagram: return "datagram";
        case Identity::Stream: return "stream";
        case Identity::UnknownContext: return "unknown-context";
        case Identity::StaleGeneration: return "stale-generation";
        default: return "unknown-transport";
    }
}

struct Report {
    uint32_t generation = 0;

    // mbedtls_ssl_handshake, DATAGRAM identities only
    uint32_t hs_calls = 0;
    uint32_t dtls_hs_calls = 0;
    uint32_t hs_ok = 0;
    uint32_t hs_fail = 0;
    int hs_last_ret = 0;
    int64_t hs_last_ms = 0;
    int64_t hs_total_ms = 0;

    // DTLS records seen on the socket
    uint32_t tx_records = 0;
    uint32_t rx_records = 0;
    uint32_t tx_unparsable = 0;     // not a DTLS record at all
    uint32_t tx_truncated = 0;      // header present, declared length exceeds data
    uint32_t tx_short_write = 0;    // real call returned < requested
    uint32_t tx_failed = 0;         // real call returned < 0
    uint32_t tx_records_in_dgram = 0;  // total records parsed across datagrams
    uint32_t rx_truncated = 0;
    uint32_t rx_unparsable = 0;
    uint32_t tx_epoch_changes = 0;
    uint32_t rx_epoch_changes = 0;
    uint16_t last_tx_epoch = 0xFFFF;
    uint16_t last_rx_epoch = 0xFFFF;
    uint32_t last_tx_seq = 0;
    uint32_t last_tx_seq_lo = 0;
    uint32_t last_rx_seq = 0;
    uint32_t last_rx_seq_lo = 0;
    uint32_t last_tx_len = 0;
    uint32_t last_rx_len = 0;
    uint8_t last_tx_type = 0xFF;
    uint8_t last_rx_type = 0xFF;
    // First record of each direction, for the handshake-stage picture.
    bool have_first_tx = false;
    bool have_first_rx = false;
    uint8_t first_tx_type = 0xFF;
    uint8_t first_rx_type = 0xFF;

    // All of these come from public API: mbedtls_ssl_get_ciphersuite,
    // mbedtls_ssl_conf_get_endpoint (via mbedtls_ssl_context_get_config),
    // mbedtls_ssl_is_handshake_over, and
    // mbedtls_ssl_get_dtls_srtp_negotiation_result whose documented out-parameter
    // is mbedtls_dtls_srtp_info. Reading that struct's documented field is using
    // the public contract, not guessing at private layout.
    bool have_cipher = false;
    char cipher[64] = {0};
    bool have_role = false;
    int role = -1;                 // MBEDTLS_SSL_IS_CLIENT / SERVER
    bool handshake_over = false;   // mbedtls_ssl_is_handshake_over()
    bool have_profile = false;
    int profile = -1;              // chosen_dtls_srtp_profile (SRTP protection profile)
    bool profile_unset = false;
    /** DTLS contexts only: STREAM (HTTPS) handshakes are counted separately. */
    uint32_t stream_hs_excluded = 0;
    uint32_t unknown_transport = 0;
    /** Identity outcomes that are NOT a transport verdict, reported separately. */
    uint32_t unknown_ctx = 0;
    uint32_t stale_ctx = 0;
    int last_identity = -1;        // Identity enum of the most recent handshake
    /** Profile reads actually performed vs. gated off. */
    uint32_t profile_reads = 0;
    uint32_t profile_skipped = 0;

    /** Setup identities seen, to prove the DATAGRAM/STREAM split is real. */
    uint32_t setups = 0;
    uint32_t datagram_setups = 0;
    uint32_t stream_setups = 0;
    uint32_t unknown_setups = 0;
    uint32_t setup_overflow = 0;   // fixed setup table full: explicit, never silent
    /** Contexts bound by a successful setup, and that table's overflow. */
    uint32_t ctx_bound = 0;
    uint32_t ctx_overflow = 0;

    /** Proof that each wrapper was actually reached, not merely defined. */
    uint32_t conf_transport_calls = 0;
    uint32_t config_defaults_calls = 0;
    uint32_t setup_calls = 0;
    uint32_t config_free_calls = 0;
    uint32_t ssl_free_calls = 0;
    /** A transport already registered for a configuration was changed. */
    uint32_t transport_updates = 0;

    /** errno discipline evidence: saved before the real call / left by it. */
    int errno_entry_last = 0;
    int errno_real_last = 0;

    /** Transport.poll() enter->exit duration, monotonic. */
    uint32_t poll_samples = 0;
    int64_t poll_last_us = 0;
    int64_t poll_max_us = 0;

    uint32_t sendto_calls = 0;
    uint32_t recvfrom_calls = 0;
    /** Socket/context association: equal / different / unknown, anonymous ids. */
    int fd_assoc = -1;              // 1 equal, 0 different, -1 unknown
    int dtls_fd = -1;
    int rtp_fd = -1;
};

struct SetupEntry {
    const mbedtls_ssl_config* conf = nullptr;
    int transport = kTransportUnknown;
    int role = kRoleUnknown;
};

/**
 * A context registration only ever comes from a successful `mbedtls_ssl_setup`.
 * The generation stamp makes an entry left over from a previous Arm() usable for
 * nothing: a stale registration never classifies a call.
 */
struct CtxEntry {
    const mbedtls_ssl_context* ctx = nullptr;
    const mbedtls_ssl_config* conf = nullptr;
    int transport = kTransportUnknown;
    int role = kRoleUnknown;
    uint32_t generation = 0;
};

constexpr size_t kSetupTable = 8;
constexpr size_t kCtxTable = 12;

struct State {
    std::mutex mtx;
    Report rep;
    uint32_t generation = 0;
    SetupEntry setups[kSetupTable];
    CtxEntry ctxs[kCtxTable];
};

inline State& Global()
{
    static State state;
    return state;
}

/** Register or update a configuration's transport/role by opaque identity. */
inline void RegisterSetupLocked(State& st, const mbedtls_ssl_config* conf,
                                int transport, int role)
{
    if (conf == nullptr) {
        return;
    }
    for (size_t i = 0; i < kSetupTable; ++i) {
        if (st.setups[i].conf == conf) {
            // An updated transport must be recorded, not dropped: esp_peer calls
            // config_defaults() and then mbedtls_ssl_conf_transport() on the same
            // configuration, and dtls_srtp_reset_session() calls it again.
            if (transport != kTransportUnknown && transport != st.setups[i].transport) {
                st.setups[i].transport = transport;
                ++st.rep.transport_updates;
            }
            if (role != kRoleUnknown) {
                st.setups[i].role = role;
            }
            return;
        }
    }
    for (size_t i = 0; i < kSetupTable; ++i) {
        if (st.setups[i].conf == nullptr) {
            st.setups[i].conf = conf;
            st.setups[i].transport = transport;
            st.setups[i].role = role;
            ++st.rep.setups;
            if (transport == kTransportDatagram) {
                ++st.rep.datagram_setups;
            } else if (transport == kTransportStream) {
                ++st.rep.stream_setups;
            } else {
                ++st.rep.unknown_setups;
            }
            return;
        }
    }
    ++st.rep.setup_overflow;
}

inline void ForgetSetupLocked(State& st, const mbedtls_ssl_config* conf)
{
    for (size_t i = 0; i < kSetupTable; ++i) {
        if (st.setups[i].conf == conf) {
            st.setups[i] = SetupEntry();
            return;
        }
    }
}

/** Bind a context to its configuration: only from a successful setup. */
inline void BindContextLocked(State& st, const mbedtls_ssl_context* ctx,
                              const mbedtls_ssl_config* conf)
{
    if (ctx == nullptr) {
        return;
    }
    int transport = kTransportUnknown;
    int role = kRoleUnknown;
    for (size_t i = 0; i < kSetupTable; ++i) {
        if (conf != nullptr && st.setups[i].conf == conf) {
            transport = st.setups[i].transport;
            role = st.setups[i].role;
            break;
        }
    }
    for (size_t i = 0; i < kCtxTable; ++i) {
        // esp_peer reuses the same context across free -> init -> setup, so a
        // re-bind must replace the previous identity of that pointer.
        if (st.ctxs[i].ctx == ctx) {
            st.ctxs[i].conf = conf;
            st.ctxs[i].transport = transport;
            st.ctxs[i].role = role;
            st.ctxs[i].generation = st.generation;
            return;
        }
    }
    for (size_t i = 0; i < kCtxTable; ++i) {
        if (st.ctxs[i].ctx == nullptr) {
            st.ctxs[i].ctx = ctx;
            st.ctxs[i].conf = conf;
            st.ctxs[i].transport = transport;
            st.ctxs[i].role = role;
            st.ctxs[i].generation = st.generation;
            ++st.rep.ctx_bound;
            return;
        }
    }
    ++st.rep.ctx_overflow;
}

/** End a context's identity when mbedtls_ssl_free() releases it. */
inline void ForgetContextLocked(State& st, const mbedtls_ssl_context* ctx)
{
    for (size_t i = 0; i < kCtxTable; ++i) {
        if (st.ctxs[i].ctx == ctx) {
            st.ctxs[i] = CtxEntry();
            return;
        }
    }
}

/** New generation: no old identity may survive into the new report. */
inline void ClearIdentityLocked(State& st)
{
    for (size_t i = 0; i < kSetupTable; ++i) {
        st.setups[i] = SetupEntry();
    }
    for (size_t i = 0; i < kCtxTable; ++i) {
        st.ctxs[i] = CtxEntry();
    }
}

/**
 * @brief Classify one handshake call.
 *
 * The (context, configuration) pair recorded by the same successful setup is the
 * only admissible evidence. A configuration that happens to be in the setup table
 * is never enough on its own - that fallback is exactly what made the old probe
 * report an HTTPS handshake as DTLS.
 */
inline Identity ClassifyLocked(const State& st, const mbedtls_ssl_context* ctx,
                               const mbedtls_ssl_config* conf_now,
                               int* role_out, int* transport_out)
{
    if (role_out != nullptr) {
        *role_out = kRoleUnknown;
    }
    if (transport_out != nullptr) {
        *transport_out = kTransportUnknown;
    }
    if (ctx == nullptr) {
        return Identity::UnknownContext;
    }
    const CtxEntry* entry = nullptr;
    for (size_t i = 0; i < kCtxTable; ++i) {
        if (st.ctxs[i].ctx == ctx) {
            entry = &st.ctxs[i];
            break;
        }
    }
    if (entry == nullptr) {
        return Identity::UnknownContext;
    }
    if (entry->generation != st.generation) {
        return Identity::StaleGeneration;
    }
    if (entry->conf != conf_now || conf_now == nullptr) {
        // The live context no longer matches the configuration it was set up
        // with: the recorded transport cannot be claimed for this call.
        return Identity::UnknownTransport;
    }
    int transport = entry->transport;
    int role = entry->role;
    if (transport == kTransportUnknown) {
        // Late registration only: still keyed on the same configuration handle.
        for (size_t i = 0; i < kSetupTable; ++i) {
            if (st.setups[i].conf == conf_now) {
                transport = st.setups[i].transport;
                if (role == kRoleUnknown) {
                    role = st.setups[i].role;
                }
                break;
            }
        }
    }
    if (transport == kTransportDatagram) {
        if (role_out != nullptr) { *role_out = role; }
        if (transport_out != nullptr) { *transport_out = transport; }
        return Identity::Datagram;
    }
    if (transport == kTransportStream) {
        if (role_out != nullptr) { *role_out = role; }
        if (transport_out != nullptr) { *transport_out = transport; }
        return Identity::Stream;
    }
    return Identity::UnknownTransport;
}

/**
 * The SRTP profile may be read only after *that* DATAGRAM handshake completed.
 * A failed or still-running handshake leaves the profile unread and counted as
 * skipped, so "profile unset" can never be manufactured by an incomplete run.
 */
inline bool ShouldReadProfile(Identity id, int rc, bool handshake_over)
{
    return id == Identity::Datagram && rc == 0 && handshake_over;
}

struct ProfileObservation {
    bool read = false;
    int profile = -1;
    bool unset = false;
};

inline ProfileObservation DecideProfile(Identity id, int rc, bool handshake_over,
                                        int chosen_profile)
{
    ProfileObservation out;
    if (!ShouldReadProfile(id, rc, handshake_over)) {
        return out;
    }
    out.read = true;
    out.profile = chosen_profile;
    out.unset = (chosen_profile == (int)MBEDTLS_TLS_SRTP_UNSET);
    return out;
}

/** Public entry points around the above; each takes the state lock. */
inline void RegisterSetup(const mbedtls_ssl_config* conf, int transport, int role)
{
    State& st = Global();
    std::lock_guard<std::mutex> lock(st.mtx);
    RegisterSetupLocked(st, conf, transport, role);
}

inline void ForgetSetup(const mbedtls_ssl_config* conf)
{
    State& st = Global();
    std::lock_guard<std::mutex> lock(st.mtx);
    ForgetSetupLocked(st, conf);
}

inline void ForgetContext(const mbedtls_ssl_context* ctx)
{
    State& st = Global();
    std::lock_guard<std::mutex> lock(st.mtx);
    ForgetContextLocked(st, ctx);
}

inline Identity Classify(const mbedtls_ssl_context* ctx,
                         const mbedtls_ssl_config* conf_now,
                         int* role_out, int* transport_out)
{
    State& st = Global();
    std::lock_guard<std::mutex> lock(st.mtx);
    return ClassifyLocked(st, ctx, conf_now, role_out, transport_out);
}

/**
 * Getters the handshake wrapper needs, injected so this header stays free of any
 * assumption about Mbed TLS object layout. The device passes the real public
 * functions; the offline check passes stubs that record whether they were called.
 */
struct HandshakeGetters {
    bool (*handshake_over)(mbedtls_ssl_context* ssl) = nullptr;
    const char* (*ciphersuite)(mbedtls_ssl_context* ssl) = nullptr;
    /** Returns MBEDTLS_TLS_SRTP_UNSET when no SRTP profile was negotiated. */
    int (*srtp_profile)(mbedtls_ssl_context* ssl) = nullptr;
};

/**
 * @brief Observe one `mbedtls_ssl_handshake()` call.
 *
 * Return value and errno are passed through untouched: errno is saved before the
 * real call, saved again immediately after it, and the post-call value is what
 * the caller sees once every getter and counter below has run.
 */
inline int WrapHandshake(mbedtls_ssl_context* ssl, const mbedtls_ssl_config* conf_now,
                         const HandshakeGetters& get, int64_t (*now_us)())
{
    const int entry_errno = errno;
    const int64_t t0 = (now_us != nullptr) ? now_us() : 0;
    const int rc = __real_mbedtls_ssl_handshake(ssl);
    const int real_errno = errno;                 // immediately after the real call
    const int64_t elapsed_ms = ((now_us != nullptr) ? now_us() : 0) - t0;

    Identity id = Identity::UnknownContext;
    int role = kRoleUnknown;
    {
        State& st = Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        id = ClassifyLocked(st, ssl, conf_now, &role, nullptr);
    }

    // Public getters, DATAGRAM identities only. Nothing here reads private
    // memory: mbedtls_ssl_is_handshake_over / mbedtls_ssl_get_ciphersuite are
    // public, and mbedtls_ssl_get_dtls_srtp_negotiation_result fills a public
    // struct whose documented field is read.
    bool over = false;
    bool have_cipher = false;
    char cipher[64] = {0};
    ProfileObservation prof;
    if (id == Identity::Datagram) {
        if (get.handshake_over != nullptr) {
            over = get.handshake_over(ssl);
        }
        if (get.ciphersuite != nullptr) {
            const char* cs = get.ciphersuite(ssl);
            if (cs != nullptr) {
                have_cipher = true;
                strncpy(cipher, cs, sizeof(cipher) - 1);
            }
        }
        if (ShouldReadProfile(id, rc, over) && get.srtp_profile != nullptr) {
            prof = DecideProfile(id, rc, over, get.srtp_profile(ssl));
        }
    }

    {
        State& st = Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        Report& r = st.rep;
        ++r.hs_calls;
        r.last_identity = (int)id;
        r.errno_entry_last = entry_errno;
        r.errno_real_last = real_errno;
        switch (id) {
            case Identity::Datagram:
                ++r.dtls_hs_calls;
                if (rc == 0) { ++r.hs_ok; } else { ++r.hs_fail; }
                r.hs_last_ret = rc;
                r.hs_last_ms = elapsed_ms / 1000;
                r.hs_total_ms += r.hs_last_ms;
                r.handshake_over = over;
                if (role != kRoleUnknown) {
                    r.have_role = true;
                    r.role = role;
                }
                if (have_cipher) {
                    r.have_cipher = true;
                    strncpy(r.cipher, cipher, sizeof(r.cipher) - 1);
                    r.cipher[sizeof(r.cipher) - 1] = '\0';
                }
                if (prof.read) {
                    ++r.profile_reads;
                    r.have_profile = true;
                    r.profile = prof.profile;
                    r.profile_unset = prof.unset;
                } else {
                    ++r.profile_skipped;
                }
                break;
            case Identity::Stream:
                ++r.stream_hs_excluded;
                break;
            case Identity::UnknownContext:
                ++r.unknown_ctx;
                break;
            case Identity::StaleGeneration:
                ++r.stale_ctx;
                break;
            default:
                ++r.unknown_transport;
                break;
        }
    }

    errno = real_errno;
    return rc;
}

/** `mbedtls_ssl_conf_transport` really returns void (ssl.h:2143). */
inline void WrapConfTransport(mbedtls_ssl_config* conf, int transport)
{
    const int entry_errno = errno;
    __real_mbedtls_ssl_conf_transport(conf, transport);
    const int real_errno = errno;
    {
        State& st = Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        ++st.rep.conf_transport_calls;
        st.rep.errno_entry_last = entry_errno;
        st.rep.errno_real_last = real_errno;
        // Updating an existing configuration's transport must be recorded.
        RegisterSetupLocked(st, conf, transport, kRoleUnknown);
    }
    errno = real_errno;
}

inline int WrapConfigDefaults(mbedtls_ssl_config* conf, int endpoint,
                              int transport, int preset)
{
    const int entry_errno = errno;
    const int rc = __real_mbedtls_ssl_config_defaults(conf, endpoint, transport, preset);
    const int real_errno = errno;
    {
        State& st = Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        ++st.rep.config_defaults_calls;
        st.rep.errno_entry_last = entry_errno;
        st.rep.errno_real_last = real_errno;
        if (rc == 0) {
            RegisterSetupLocked(st, conf, transport, endpoint);
        }
    }
    errno = real_errno;
    return rc;
}

inline int WrapSetup(mbedtls_ssl_context* ssl, const mbedtls_ssl_config* conf)
{
    const int entry_errno = errno;
    const int rc = __real_mbedtls_ssl_setup(ssl, conf);
    const int real_errno = errno;
    {
        State& st = Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        ++st.rep.setup_calls;
        st.rep.errno_entry_last = entry_errno;
        st.rep.errno_real_last = real_errno;
        if (rc == 0) {
            BindContextLocked(st, ssl, conf);
        }
    }
    errno = real_errno;
    return rc;
}

/** Identity ends when the configuration is released (mbedtls_ssl_config_free). */
inline void WrapConfigFree(mbedtls_ssl_config* conf)
{
    const int entry_errno = errno;
    __real_mbedtls_ssl_config_free(conf);
    const int real_errno = errno;
    {
        State& st = Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        ++st.rep.config_free_calls;
        st.rep.errno_entry_last = entry_errno;
        st.rep.errno_real_last = real_errno;
        ForgetSetupLocked(st, conf);
    }
    errno = real_errno;
}

/** Identity ends when the context is released (mbedtls_ssl_free). */
inline void WrapSslFree(mbedtls_ssl_context* ssl)
{
    const int entry_errno = errno;
    __real_mbedtls_ssl_free(ssl);
    const int real_errno = errno;
    {
        State& st = Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        ++st.rep.ssl_free_calls;
        st.rep.errno_entry_last = entry_errno;
        st.rep.errno_real_last = real_errno;
        ForgetContextLocked(st, ssl);
    }
    errno = real_errno;
}

/** Start a new generation; no identity from the previous one may survive. */
inline void Arm()
{
    State& st = Global();
    std::lock_guard<std::mutex> lock(st.mtx);
    const uint32_t next = st.generation + 1;   // never restarts at 1
    st.rep = Report();
    st.rep.generation = next;
    st.generation = next;
    ClearIdentityLocked(st);
}

inline Report Snapshot()
{
    State& st = Global();
    std::lock_guard<std::mutex> lock(st.mtx);
    // Association is measured, not assumed: equal / different / unknown.
    if (st.rep.dtls_fd >= 0 && st.rep.rtp_fd >= 0) {
        st.rep.fd_assoc = (st.rep.dtls_fd == st.rep.rtp_fd) ? 1 : 0;
    } else {
        st.rep.fd_assoc = -1;
    }
    return st.rep;
}

}  // namespace dtls_probe
