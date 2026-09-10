/*
 * test_connect_observability.c — proof harness for #36 (netplay connect
 * observability).
 *
 * WHAT THIS EXISTS TO PROVE. #36 is not a feature, it is a claim about
 * evidence: that when a connection fails in the field, the file a tester
 * sends us actually contains the facts needed to attribute the failure,
 * and does NOT contain a confident verdict the evidence cannot support.
 * A harness that only unit-tested the string formatter would prove
 * neither half. So every test here INDUCES the condition — a real
 * version-skewed frame on a real loopback socket, a real silent server,
 * real worker threads hammering the log sink — and then reads back what
 * was actually written to disk.
 *
 * The principle cuts BOTH ways, and the tests below are built around
 * that: where the evidence is genuinely ambiguous the report must say
 * so, and where it is definite the report must not hedge either. A
 * hedge printed over measured proof buries the one case the measurement
 * exists to attribute.
 *
 * Covers:
 *   1. Version-skew detection on a real wire, WITH the negative case
 *      (a magic-matched runt must NOT be counted as a skew), and the
 *      induced frame carried through to a DEFINITE version-skew verdict.
 *   2. Silent rendezvous => the failure is reported AMBIGUOUS, never
 *      attributed (the #87 v1-server case is indistinguishable from an
 *      unreachable server, and must not be claimed otherwise) — plus the
 *      opposite polarity on the same code.
 *   3. A confirmed punch is RECORDED even though the race may discard
 *      it, and the pure attribution helper refuses to call that a NAT
 *      failure — for EVERY code a confirm contradicts, and pointedly not
 *      for COOKIE_REJECTED, which it does not. See the long note in
 *      test3 for why the "confirmed AND EXHAUSTED" state is unreachable
 *      in-process by construction.
 *   4. The MT log sink really lands 800 lines from 4 threads in the
 *      file, untorn. Everything else rides on this mechanism.
 *   5. The S2 join retry starts from zero evidence, so attempt 2 can
 *      never report attempt 1's counters. See test5 for exactly what
 *      about the two-attempt path could not be induced offline.
 *
 * And, for #44 — the evidence is only useful if it is still on the
 * device and small enough to send:
 *   6. The per-session byte budget really freezes the file, once, with
 *      one marker, instead of merely claiming to.
 *   7. The prune keeps the 20 newest session logs and — the assertion
 *      that matters — does NOT match netplay-report.txt,
 *      netplay-report.1.txt or an unrelated bystander file.
 *   8. The tester report rotates at two generations and carries ONLY
 *      the attributed "[netplay-connect]" one-liners, stamped.
 *
 * Gated behind ENABLE_NETPLAY_TESTS. Enable with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS='-DENABLE_NETPLAY_TESTS -DNETPLAY_TEST_HOOKS'"
 * Mirrors the src/netplay/test_room_code.c pattern.
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/connect_fail.h"
#include "netplay/direct_p2p.h"
#include "netplay/netplay.h"
#include "netplay/rendezvous.h"
#include "netplay/stun.h"
#include "port/paths.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

static int fail_count = 0;

static void fail(const char* tag, const char* why) {
    fprintf(stderr, "[test_connect_observability] FAIL: %s: %s\n", tag, why);
    fail_count++;
}

#define EXPECT_TRUE(tag, cond)                                                     \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fail((tag), "expected true: " #cond);                                  \
        }                                                                          \
    } while (0)

static void pass(const char* tag, const char* what) {
    fprintf(stderr, "[test_connect_observability] PASS: %s: %s\n", tag, what);
}

/* ======================================================================
 * The per-session netplay log file, as a readable artifact
 * ======================================================================
 *
 * netplay.c opens <PrefPath>logs/netplay-<utc_ms>.log lazily on the FIRST
 * connect-event and only closes it from a live session's teardown paths —
 * neither of which this harness reaches. So exactly ONE new file appears
 * for the whole run, and every MT line lands in it. We snapshot the
 * newest pre-existing timestamp before doing anything, then identify our
 * file as "the one with a larger timestamp", and delete it at the end.
 * That avoids ever touching a file this harness did not create. */

static void obs_logs_dir(char* out, size_t cap) {
    const char* pref = Paths_GetPrefPath();
    SDL_snprintf(out, cap, "%slogs", (pref != NULL) ? pref : "");
}

/* Parse "netplay-<digits>.log" -> <digits>. 0 when the name does not
 * match BOTH the prefix and the suffix. */
static unsigned long long obs_parse_log_ts(const char* name) {
    static const char k_prefix[] = "netplay-";
    static const char k_suffix[] = ".log";
    const size_t nlen = strlen(name);
    const size_t plen = sizeof(k_prefix) - 1;
    const size_t slen = sizeof(k_suffix) - 1;
    if (nlen <= plen + slen) {
        return 0;
    }
    if (strncmp(name, k_prefix, plen) != 0) {
        return 0;
    }
    if (strcmp(name + nlen - slen, k_suffix) != 0) {
        return 0;
    }
    /* Digits only — never trust a filename's shape from a shared dir. */
    for (size_t i = plen; i < nlen - slen; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
    }
    return strtoull(name + plen, NULL, 10);
}

static unsigned long long obs_newest_log_ts(void) {
    char dir[512];
    obs_logs_dir(dir, sizeof(dir));
    DIR* d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    unsigned long long best = 0;
    struct dirent* e;
    int examined = 0;
    while ((e = readdir(d)) != NULL && examined < 4096) {
        examined++;
        const unsigned long long ts = obs_parse_log_ts(e->d_name);
        if (ts > best) {
            best = ts;
        }
    }
    closedir(d);
    return best;
}

/* Full path of the single log file this run created, or false when the
 * sink never opened one. */
/* #125. The session log THIS process opened, asked of the module that
 * opened it, with the directory scan kept only as the fallback for the
 * case the hook is built out.
 *
 * The scan below is what made #125 possible: it takes the NEWEST
 * netplay-<ms>.log in <PrefPath>logs with a stamp past `since_ts`, and
 * under concurrent gate runs that file belongs to another process.
 * Measured with four concurrent runs of this harness: three of them wrote
 * to netplay-1788111677761.log and all three read back
 * netplay-1788111677763.log, then reported "800 of 800 MT lines missing
 * or torn" and "the session file grew AFTER the TRUNCATED marker" about a
 * file they had never written a line to. Both diagnoses were about the
 * wrong file; nothing was wrong with the sink.
 *
 * Per-process isolation (Paths_GetPrefPath honouring THIRDSARM_HOME on
 * every port now, plus tools/gates/run-gates.sh handing each harness its
 * own home) is what actually removes the sharing. This makes the harness
 * correct even without it. */
static bool obs_find_new_log(unsigned long long since_ts, char* out, size_t cap) {
#ifdef NETPLAY_TEST_HOOKS
    if (Netplay_TestHook_SessionLogPath(out, cap)) {
        return true;
    }
#endif
    char dir[512];
    obs_logs_dir(dir, sizeof(dir));
    DIR* d = opendir(dir);
    if (d == NULL) {
        return false;
    }
    unsigned long long best = 0;
    char best_name[256] = { 0 };
    struct dirent* e;
    int examined = 0;
    while ((e = readdir(d)) != NULL && examined < 4096) {
        examined++;
        const unsigned long long ts = obs_parse_log_ts(e->d_name);
        if (ts > since_ts && ts > best) {
            best = ts;
            SDL_strlcpy(best_name, e->d_name, sizeof(best_name));
        }
    }
    closedir(d);
    if (best == 0) {
        return false;
    }
    SDL_snprintf(out, cap, "%s/%s", dir, best_name);
    return true;
}

/* Slurp the file. Caller frees. NULL on any failure. */
static char* obs_read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char* buf = (char*)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len != NULL) {
        *out_len = got;
    }
    return buf;
}

/* ======================================================================
 * A rendezvous-shaped server that answers with frames we cannot parse
 * ====================================================================== */

typedef enum {
    OBS_REPLY_BADVER = 0, /* '3SXR' + version byte 1 + type DELIVER + pad */
    OBS_REPLY_RUNT,       /* '3SXR' + one byte: magic matches, len < 6    */
    OBS_REPLY_NACK        /* #122: a real 28-byte typed refusal            */
} ObsReplyMode;

typedef struct {
    int             sock;
    ObsReplyMode    mode;
    /* --- #122, OBS_REPLY_NACK only ---------------------------------- */
    uint8_t         nack_reason;  /* the reason byte to send             */
    bool            nack_bad_key; /* corrupt the echoed session key      */
    int             alt_sock;     /* > 0: reply from THIS socket instead,
                                     which puts the NACK on a different
                                     source PORT and is how the source
                                     gate gets a negative control. Zero,
                                     not -1, is "unset" so the memset-0
                                     initialisation every other ctx in
                                     this file uses stays correct — fd 0
                                     is stdin and is never a socket here. */
    SDL_AtomicInt   stop;
    SDL_AtomicInt   replies;
} ObsServerCtx;

static int obs_server_thread(void* arg) {
    ObsServerCtx* ctx = (ObsServerCtx*)arg;
    for (;;) {
        if (SDL_GetAtomicInt(&ctx->stop)) {
            break;
        }
        uint8_t buf[512];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        memset(&src, 0, sizeof(src));
        const ssize_t n = recvfrom(ctx->sock, buf, sizeof(buf), 0,
                                   (struct sockaddr*)&src, &sl);
        if (n <= 0) {
            SDL_Delay(2);
            continue;
        }
        /* Hand-built by byte, on purpose: Rendezvous_BuildRegister and
         * friends can only ever emit OUR version, so the only way to put
         * a foreign-version frame on the wire is to write one. */
        uint8_t reply[32];
        memset(reply, 0, sizeof(reply));
        reply[0] = 0x33; /* '3' */
        reply[1] = 0x53; /* 'S' */
        reply[2] = 0x58; /* 'X' */
        reply[3] = 0x52; /* 'R' */
        size_t reply_len;
        if (ctx->mode == OBS_REPLY_NACK) {
            /* #122. Byte-for-byte encodeNack() from
             * tools/rendezvous-server/rendezvous-server.js:856, built
             * here rather than through any client helper because the
             * client HAS no NACK encoder — only the server emits one, and
             * a mock that shared code with the parser would be testing
             * the parser against itself.
             *
             * The version is Rendezvous_WireVersion(), NOT a literal: a
             * real server stamps the NACK with the version it speaks, and
             * the case under test is a server that speaks OURS. (The
             * foreign-version case is test1's badver arm and reaches a
             * different branch entirely.) */
            reply[4] = (uint8_t)Rendezvous_WireVersion();
            reply[5] = (uint8_t)REND_FRAME_NACK;
            reply[6] = ctx->nack_reason;
            reply[7] = 0; /* reserved */
            /* The server ECHOES the sender's own session key from
             * [8..24]; nothing server-side is disclosed. */
            if (n >= 24) {
                memcpy(&reply[8], &buf[8], 16);
                if (ctx->nack_bad_key) {
                    reply[8 + 15] ^= 0x01u; /* one bit — the strongest form */
                }
            }
            reply_len = 28; /* REND_NACK_LEN */
        } else if (ctx->mode == OBS_REPLY_BADVER) {
            /* Version 1: deliberately NOT Rendezvous_WireVersion(). If
             * this build's wire version ever became 1, the frame would
             * stop being a skew and this test would (correctly) fail. */
            reply[4] = 1u;
            reply[5] = (uint8_t)REND_FRAME_DELIVER;
            reply_len = sizeof(reply);
        } else {
            /* 5 bytes: passes Rendezvous_HasMagic (needs len >= 4) but
             * is rejected by Rendezvous_FrameType for being too short to
             * carry a type (rendezvous.c:341-343) — the SAME ft == 0 the
             * skew arm sees. This is the negative case: a runt must not
             * be miscounted as a protocol skew. */
            reply[4] = 1u;
            reply_len = 5;
        }
        (void)sendto((ctx->alt_sock > 0) ? ctx->alt_sock : ctx->sock,
                     reply, reply_len, 0, (struct sockaddr*)&src, sl);
        SDL_AddAtomicInt(&ctx->replies, 1);
    }
    return 0;
}

static int obs_open_udp_loopback(unsigned short* out_port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return -1;
    }
    int one = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    /* 127.0.0.1 written out rather than INADDR_LOOPBACK: the macro is
     * behind a feature-test gate that this TU's include order does not
     * satisfy, and the literal is what the rest of the tree uses. */
    a.sin_addr.s_addr = htonl(0x7F000001u);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        close(s);
        return -1;
    }
    /* Non-blocking recvfrom via a short timeout, so the server thread can
     * observe its stop flag instead of parking forever in recvfrom. */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 20000;
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    socklen_t sl = sizeof(a);
    if (getsockname(s, (struct sockaddr*)&a, &sl) != 0) {
        close(s);
        return -1;
    }
    *out_port = ntohs(a.sin_port);
    return s;
}

/* The joiner's own socket for a probe race. */
static NET_DatagramSocket* obs_open_client_socket(uint16_t* out_port) {
    NET_Address* bind_addr = NET_ResolveHostname("127.0.0.1");
    if (bind_addr != NULL) {
        int wait = 0;
        while (NET_GetAddressStatus(bind_addr) == NET_WAITING && wait < 200) {
            SDL_Delay(1);
            wait++;
        }
    }
    NET_DatagramSocket* s = NET_CreateDatagramSocket(bind_addr, 0);
    if (bind_addr != NULL) {
        NET_UnrefAddress(bind_addr);
    }
    if (s == NULL) {
        return NULL;
    }
    *out_port = 40000; /* claimed public port; nothing here validates it */
    return s;
}

static const uint8_t k_obs_key[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
};
static const uint8_t k_obs_token[STUN_PUNCH_TOKEN_LEN] = {
    0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8
};

#define OBS_RACE_BUDGET_MS 1400

/* Run one signal-only race against `signal_port`. No punch legs are
 * armed (seed_port 0), so the race is purely the rendezvous
 * conversation — which is exactly the leg under test. */
static bool obs_run_signal_race_ms(uint16_t signal_port, int budget_ms,
                                   DirectP2PRaceProbeOut* out) {
    uint16_t my_pub = 0;
    NET_DatagramSocket* sock = obs_open_client_socket(&my_pub);
    if (sock == NULL) {
        return false;
    }
    DirectP2PRaceProbeCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host_role = false;
    cfg.sock = sock;
    cfg.punch_token = k_obs_token;
    cfg.seed_ip = "127.0.0.1";
    cfg.seed_port = 0; /* no seed punch leg */
    cfg.signal_ip = "127.0.0.1";
    cfg.signal_port = signal_port;
    cfg.session_key = k_obs_key;
    cfg.my_public_port = my_pub;
    cfg.signal_leg = true;
    cfg.signal_budget_ms = budget_ms;
    cfg.punch_leg_ms = 100;
    cfg.race_budget_ms = budget_ms;

    DirectP2P_TestHook_RunRace(&cfg, out);
    NET_DestroyDatagramSocket(sock);
    return true;
}

static bool obs_run_signal_race(uint16_t signal_port, DirectP2PRaceProbeOut* out) {
    return obs_run_signal_race_ms(signal_port, OBS_RACE_BUDGET_MS, out);
}

/* ======================================================================
 * Test 1 — version skew, induced on a real socket, with its negative
 * ====================================================================== */

static void test1_version_skew(unsigned long long before_ts) {
    const char* tag = "test1-version-skew";

    /* --- 1a. the NEGATIVE case first: magic-matched runts ------------- */
    unsigned short runt_port = 0;
    int runt_sock = obs_open_udp_loopback(&runt_port);
    if (runt_sock < 0) {
        fail(tag, "could not bind the runt server socket");
        return;
    }
    ObsServerCtx runt_ctx;
    memset(&runt_ctx, 0, sizeof(runt_ctx));
    runt_ctx.sock = runt_sock;
    runt_ctx.mode = OBS_REPLY_RUNT;
    SDL_SetAtomicInt(&runt_ctx.stop, 0);
    SDL_Thread* runt_tid = SDL_CreateThread(obs_server_thread, "obs_runt", &runt_ctx);
    if (runt_tid == NULL) {
        close(runt_sock);
        fail(tag, "SDL_CreateThread failed for the runt server");
        return;
    }

    DirectP2PRaceProbeOut runt_out;
    memset(&runt_out, 0, sizeof(runt_out));
    const bool runt_ran = obs_run_signal_race(runt_port, &runt_out);

    SDL_SetAtomicInt(&runt_ctx.stop, 1);
    SDL_WaitThread(runt_tid, NULL);
    close(runt_sock);

    if (!runt_ran) {
        fail(tag, "could not open the client socket for the runt race");
    } else {
        EXPECT_TRUE(tag, SDL_GetAtomicInt(&runt_ctx.replies) > 0);
        /* THE DISCRIMINATOR. Rendezvous_FrameType returns 0 for a runt
         * AND for a version skew alike (rendezvous.c:341-350), so an arm
         * keyed on `ft == 0` alone would count these. Blaming "protocol
         * skew" for a truncated datagram is precisely the confident-but-
         * wrong attribution #36 exists to stop. */
        if (runt_out.badver_n != 0) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: badver_n=%u after %d "
                    "magic-matched 5-byte RUNTS — a runt is not a version skew\n",
                    tag, (unsigned)runt_out.badver_n,
                    SDL_GetAtomicInt(&runt_ctx.replies));
            fail_count++;
        } else {
            pass(tag, "a magic-matched runt is NOT counted as a protocol skew");
        }
    }

    /* --- 1b. the POSITIVE case: a real foreign-version frame ---------- */
    unsigned short bad_port = 0;
    int bad_sock = obs_open_udp_loopback(&bad_port);
    if (bad_sock < 0) {
        fail(tag, "could not bind the bad-version server socket");
        return;
    }
    ObsServerCtx bad_ctx;
    memset(&bad_ctx, 0, sizeof(bad_ctx));
    bad_ctx.sock = bad_sock;
    bad_ctx.mode = OBS_REPLY_BADVER;
    SDL_SetAtomicInt(&bad_ctx.stop, 0);
    SDL_Thread* bad_tid = SDL_CreateThread(obs_server_thread, "obs_badver", &bad_ctx);
    if (bad_tid == NULL) {
        close(bad_sock);
        fail(tag, "SDL_CreateThread failed for the bad-version server");
        return;
    }

    DirectP2PRaceProbeOut bad_out;
    memset(&bad_out, 0, sizeof(bad_out));
    const bool bad_ran = obs_run_signal_race(bad_port, &bad_out);

    SDL_SetAtomicInt(&bad_ctx.stop, 1);
    SDL_WaitThread(bad_tid, NULL);
    close(bad_sock);

    if (!bad_ran) {
        fail(tag, "could not open the client socket for the bad-version race");
        return;
    }
    if (bad_out.badver_n < 1) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: badver_n=%u after %d "
                "foreign-version '3SXR' frames — the skew detector did not fire\n",
                tag, (unsigned)bad_out.badver_n, SDL_GetAtomicInt(&bad_ctx.replies));
        fail_count++;
    } else {
        pass(tag, "a foreign-version '3SXR' frame IS counted as a protocol skew");
    }
    /* A version-skewed frame is not a DELIVER and not a CHALLENGE — it
     * must not leak into the evidence the classifier reads. */
    EXPECT_TRUE(tag, bad_out.deliver_n == 0);
    EXPECT_TRUE(tag, bad_out.challenge_n == 0);

    /* --- 1b'. F2: the INDUCED skew drives a DEFINITE verdict ----------
     *
     * This is the induced frame from 1b carried all the way through to
     * the decision a triager reads, with nothing hand-built in between:
     * the counters come off the wire, ClassifyJoin runs on them, and
     * Attribute must call the result what it is. A server that answers
     * in a protocol we cannot parse is not "we could not tell" — it is a
     * measured version mismatch with a concrete remedy. */
    if (bad_out.badver_n > 0) {
        ConnectJoinEvidence skew_ev;
        memset(&skew_ev, 0, sizeof(skew_ev));
        skew_ev.deliver_any = (bad_out.deliver_n != 0);
        skew_ev.deliver_real = false;
        skew_ev.challenge_any = (bad_out.challenge_n != 0);
        const ConnectFailCode skew_code = ConnectFail_ClassifyJoin(&skew_ev);
        if (skew_code != CONNECT_FAIL_RENDEZVOUS_DOWN) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: unparseable frames should "
                    "still classify as P2P_FAIL_RENDEZVOUS_DOWN, got %s\n",
                    tag, ConnectFail_Code(skew_code));
            fail_count++;
        }
        const ConnectAttribution skew_a =
            ConnectFail_Attribute(skew_code, false, bad_out.badver_n);
        if (skew_a != CONNECT_ATTRIB_VERSION_SKEW) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: badver_n=%u must attribute "
                    "as CONNECT_ATTRIB_VERSION_SKEW, got \"%s\"\n",
                    tag, (unsigned)bad_out.badver_n,
                    ConnectFail_AttributionText(skew_a));
            fail_count++;
        } else {
            pass(tag, "an induced skew frame yields the DEFINITE version-skew verdict");
        }
        EXPECT_TRUE(tag, strcmp(ConnectFail_AttributionText(skew_a),
                                "version-skew") == 0);
        /* The self-contradiction this finding exists to kill: a NOTE
         * calling the evidence "indistinguishable" printed directly under
         * a FAIL line that shows a non-zero skew count. */
        const char* skew_note = ConnectFail_AttributionNote(skew_a);
        if (strstr(skew_note, "indistinguishable") != NULL) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: the version-skew note calls "
                    "DEFINITE evidence indistinguishable: \"%s\"\n",
                    tag, skew_note);
            fail_count++;
        }
        EXPECT_TRUE(tag, skew_note[0] != '\0');
        EXPECT_TRUE(tag, strstr(skew_note, "version") != NULL);
    }

    /* --- 1c. and it reaches the tester's FILE, not just the console --- */
    char path[768];
    if (!obs_find_new_log(before_ts, path, sizeof(path))) {
        fail(tag, "no new netplay-*.log was created by the MT sink");
        return;
    }
    size_t len = 0;
    char* body = obs_read_file(path, &len);
    if (body == NULL) {
        fail(tag, "could not read back the netplay log file");
        return;
    }
    if (strstr(body, "badver=") == NULL) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: the race-done line in %s "
                "carries no badver= field\n",
                tag, path);
        fail_count++;
    } else {
        pass(tag, "the race-done line carrying badver= reached the log FILE");
    }
    /* The same line must carry the rest of the #36 evidence. */
    EXPECT_TRUE(tag, strstr(body, "confirm=") != NULL);
    EXPECT_TRUE(tag, strstr(body, "dgap=") != NULL);
    free(body);
}

/* ======================================================================
 * Test 2 — a silent rendezvous is AMBIGUOUS, never attributed
 * ====================================================================== */

static void test2_silent_is_ambiguous(void) {
    const char* tag = "test2-silent-ambiguous";

    /* Bind a port, learn it, close it: nothing is listening there, so
     * the race sees zero frames of any kind. That is bit-for-bit what a
     * v2 client sees against the deployed v1 server (#87), which drops a
     * version-mismatched frame with no reply at all. */
    unsigned short dead_port = 0;
    int probe = obs_open_udp_loopback(&dead_port);
    if (probe < 0) {
        fail(tag, "could not bind a socket to learn a dead port");
        return;
    }
    close(probe);

    DirectP2PRaceProbeOut out;
    memset(&out, 0, sizeof(out));
    if (!obs_run_signal_race(dead_port, &out)) {
        fail(tag, "could not open the client socket");
        return;
    }

    if (out.deliver_n != 0 || out.challenge_n != 0 || out.badver_n != 0) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: expected total silence, got "
                "deliver_n=%u challenge_n=%u badver_n=%u\n",
                tag, (unsigned)out.deliver_n, (unsigned)out.challenge_n,
                (unsigned)out.badver_n);
        fail_count++;
    } else {
        pass(tag, "a closed rendezvous port yields zero frames of every kind");
    }

    /* That silence classifies as RENDEZVOUS_DOWN... */
    ConnectJoinEvidence ev;
    memset(&ev, 0, sizeof(ev));
    ev.deliver_any = (out.deliver_n != 0);
    ev.deliver_real = false;
    ev.challenge_any = (out.challenge_n != 0);
    const ConnectFailCode code = ConnectFail_ClassifyJoin(&ev);
    if (code != CONNECT_FAIL_RENDEZVOUS_DOWN) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: ClassifyJoin returned %s, "
                "expected P2P_FAIL_RENDEZVOUS_DOWN\n",
                tag, ConnectFail_Code(code));
        fail_count++;
    }

    /* ...and #36's whole point: that code is NOT reported as certain.
     * TRUE silence — badver_n == 0, straight off the wire above — is the
     * one case where "we cannot tell" is the honest answer. */
    const ConnectAttribution a =
        ConnectFail_Attribute(code, false, out.badver_n);
    if (a != CONNECT_ATTRIB_AMBIG_VERSION) {
        fail(tag, "silence must attribute as CONNECT_ATTRIB_AMBIG_VERSION");
    } else {
        pass(tag, "silence is reported ambiguous, not attributed");
    }
    EXPECT_TRUE(tag, strcmp(ConnectFail_AttributionText(a),
                            "ambiguous-rendezvous-silent") == 0);
    const char* note = ConnectFail_AttributionNote(a);
    if (strstr(note, "version") == NULL) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: the note does not mention the "
                "version-skew alternative: \"%s\"\n",
                tag, note);
        fail_count++;
    }
    /* Here — and ONLY here — "indistinguishable" is the correct word. */
    EXPECT_TRUE(tag, strstr(note, "indistinguishable") != NULL);

    /* F2, both polarities pinned on the same code. The counter is the
     * ONLY thing that changes between these two calls, so the pair is a
     * direct statement of the rule: a witnessed skew is definite, an
     * unwitnessed silence is not. */
    if (ConnectFail_Attribute(CONNECT_FAIL_RENDEZVOUS_DOWN, false, 0u) !=
        CONNECT_ATTRIB_AMBIG_VERSION) {
        fail(tag, "RENDEZVOUS_DOWN with badver_n==0 must stay AMBIG_VERSION");
    }
    if (ConnectFail_Attribute(CONNECT_FAIL_RENDEZVOUS_DOWN, false, 1u) !=
        CONNECT_ATTRIB_VERSION_SKEW) {
        fail(tag, "RENDEZVOUS_DOWN with badver_n>0 must be VERSION_SKEW");
    }
    /* Rule ORDER: a socket-level confirm contradicts the code itself and
     * outranks a server-level skew observation. RENDEZVOUS_DOWN is not in
     * the confirm-contradiction set, so this pair also pins that the
     * confirm flag does not silently disturb the skew arm. */
    if (ConnectFail_Attribute(CONNECT_FAIL_RENDEZVOUS_DOWN, true, 1u) !=
        CONNECT_ATTRIB_VERSION_SKEW) {
        fail(tag, "RENDEZVOUS_DOWN is not confirm-contradicted; skew must still win");
    }
    /* badver_n is only consulted for RENDEZVOUS_DOWN — it says nothing
     * about a code that was never inferred from silence. */
    if (ConnectFail_Attribute(CONNECT_FAIL_STUN_ALLDOWN, false, 9u) !=
        CONNECT_ATTRIB_SUPPORTED) {
        fail(tag, "badver_n must not change the verdict for a non-silence code");
    }
    pass(tag, "a witnessed skew is definite; unwitnessed silence stays ambiguous");
}

/* ======================================================================
 * Test 3 — a confirmed punch is never reported as a NAT failure
 * ====================================================================== */

/* Oracle: confirm whatever we are asked about. */
static DirectP2PPunchOracleResult obs_oracle_confirm(const char* ip, uint16_t port) {
    (void)ip;
    (void)port;
    return DP2P_PUNCH_CONFIRM;
}

static void test3_confirm_not_nat(void) {
    const char* tag = "test3-confirm-not-nat";

    /*
     * THE INDUCED ROUTE WAS TRIED AND IS UNREACHABLE BY CONSTRUCTION.
     *
     * The spec asked for a race that CONFIRMS a punch leg and still ends
     * RACE_EXHAUSTED. That state cannot be reached in this process, and
     * the reason is a property of the shipped code, not a limitation of
     * the harness:
     *
     *   - race_punch_confirmed() returns false the moment a leg is
     *     finished (direct_p2p.c:1694-1700), and section 2's lifetime
     *     sweep `continue`s over any confirmed leg, so a confirmed leg is
     *     never finished by the budget.
     *   - The latest instant a leg can still be confirmed is therefore
     *     send_end + RACE_PUNCH_SETTLE_MS, i.e. at most
     *     budget + STUN_PUNCH_CONFIRM_MS (the two constants are asserted
     *     equal at direct_p2p.c:1517).
     *   - A confirmed leg settles one tail later, and the loop's hard cap
     *     is budget + 2 * STUN_PUNCH_CONFIRM_MS (direct_p2p.c section 8).
     *     Settling therefore always precedes the cap, except at an exact
     *     millisecond tie.
     *
     * That is the H-1 machinery working: the race is CONSTRUCTED so it
     * never throws a confirmed punch away. Asserting "confirmed AND
     * EXHAUSTED" would mean asserting a knife-edge tie, i.e. shipping a
     * flaky test. So this test does the two things that are real:
     *   (a) it INDUCES a confirm through the shipped oracle seam and
     *       proves confirm_seen / confirm_ms are actually recorded — the
     *       recording happens BEFORE the settle test on purpose
     *       (direct_p2p.c section 1), which is what would carry the
     *       evidence out if the race ever did discard a confirm; and
     *   (b) it pins the pure helper on hand-built evidence, both
     *       polarities, which is the decision that would actually be
     *       wrong in the field.
     */
    DirectP2P_TestHook_SetPunchOracle(obs_oracle_confirm);

    uint16_t my_pub = 0;
    NET_DatagramSocket* sock = obs_open_client_socket(&my_pub);
    if (sock == NULL) {
        DirectP2P_TestHook_SetPunchOracle(NULL);
        fail(tag, "could not open the client socket");
        return;
    }

    DirectP2PRaceProbeCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host_role = false;
    cfg.sock = sock;
    cfg.punch_token = k_obs_token;
    cfg.seed_ip = "127.0.0.1";
    cfg.seed_port = 9; /* discard port — the oracle decides, nothing is sent */
    cfg.signal_ip = NULL;
    cfg.signal_port = 0;
    cfg.session_key = NULL;
    cfg.my_public_port = my_pub;
    cfg.signal_leg = false;
    cfg.signal_budget_ms = 0;
    cfg.punch_leg_ms = 800;
    cfg.race_budget_ms = 800;

    DirectP2PRaceProbeOut out;
    memset(&out, 0, sizeof(out));
    DirectP2P_TestHook_RunRace(&cfg, &out);

    NET_DestroyDatagramSocket(sock);
    DirectP2P_TestHook_SetPunchOracle(NULL);

    if (!out.confirm_seen) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: a punch leg confirmed "
                "(outcome=%d) but confirm_seen was not recorded\n",
                tag, (int)out.outcome);
        fail_count++;
    } else {
        pass(tag, "a confirmed punch leg is recorded in the race evidence");
    }
    /* Report which outcome the induced route actually produced, so a
     * future reader does not have to re-derive the argument above. */
    fprintf(stderr,
            "[test_connect_observability] note: %s: induced confirm produced "
            "outcome=%d (PUNCHED=%d) confirm_ms=%u — the confirmed-AND-EXHAUSTED "
            "state is unreachable by construction; see the comment in this test.\n",
            tag, (int)out.outcome, (int)DP2P_RACE_PROBE_PUNCHED,
            (unsigned)out.confirm_ms);

    /* (b) The decision that matters, pinned on hand-built evidence.
     *
     * F3 — THE CONTRADICTION SET. `confirm_seen` means one specific
     * thing: a datagram from the peer reached our socket AND passed the
     * token check. Every code below asserts something that fact makes
     * false, so with a confirm each must go ambiguous, and without one
     * each must stay supported. Both polarities are asserted for every
     * member; a one-sided assertion would pass against a helper that
     * simply returned AMBIG_CONFIRM for everything. */
    static const ConnectFailCode k_contradicted[] = {
        CONNECT_FAIL_NAT_BLOCKED,     /* "peer's datagrams cannot reach us"  */
        CONNECT_FAIL_SYMMETRIC_BOTH,  /* same, plus a port-reassign detail   */
        CONNECT_FAIL_HOST_OFFLINE,    /* "host absent" — the seed leg
                                         confirming proves it is not        */
        CONNECT_FAIL_HAIRPIN,         /* "router lacks loopback" — it looped */
        CONNECT_FAIL_PUNCH_AUTH       /* "token check failed" — one passed   */
    };
    for (size_t i = 0; i < sizeof(k_contradicted) / sizeof(k_contradicted[0]); i++) {
        const ConnectFailCode c = k_contradicted[i];
        const ConnectAttribution with = ConnectFail_Attribute(c, true, 0u);
        const ConnectAttribution without = ConnectFail_Attribute(c, false, 0u);
        if (with != CONNECT_ATTRIB_AMBIG_CONFIRM) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: %s with a confirmed punch "
                    "must be AMBIG_CONFIRM, got \"%s\"\n",
                    tag, ConnectFail_Code(c), ConnectFail_AttributionText(with));
            fail_count++;
        }
        if (without != CONNECT_ATTRIB_SUPPORTED) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: %s without a confirmed "
                    "punch must be SUPPORTED, got \"%s\"\n",
                    tag, ConnectFail_Code(c), ConnectFail_AttributionText(without));
            fail_count++;
        }
    }
    pass(tag, "every code a confirm contradicts goes ambiguous, and only with a confirm");

    /* THE DELIBERATE EXCLUSION, asserted so a later "fix" trips here.
     * COOKIE_REJECTED is the rendezvous SERVER refusing our REGISTER
     * because the cookie echo did not verify — a client<->server fact.
     * A punch confirm is a client<->PEER fact about a different
     * conversation with a different counterparty; the two are
     * simultaneously true with no contradiction. Marking it ambiguous
     * would invent doubt about a verdict the evidence fully supports,
     * which is the same failure mode as inventing certainty. */
    if (ConnectFail_Attribute(CONNECT_FAIL_COOKIE_REJECTED, true, 0u) !=
        CONNECT_ATTRIB_SUPPORTED) {
        fail(tag, "COOKIE_REJECTED must stay SUPPORTED WITH a confirm (orthogonal)");
    }
    if (ConnectFail_Attribute(CONNECT_FAIL_COOKIE_REJECTED, false, 0u) !=
        CONNECT_ATTRIB_SUPPORTED) {
        fail(tag, "COOKIE_REJECTED must stay SUPPORTED without a confirm");
    }
    pass(tag, "COOKIE_REJECTED is orthogonal to a punch confirm and stays SUPPORTED");

    if (ConnectFail_Attribute(CONNECT_FAIL_NONE, false, 0u) != CONNECT_ATTRIB_OK) {
        fail(tag, "CONNECT_FAIL_NONE must attribute as OK");
    }
    /* Success outranks everything, including a live skew count. */
    if (ConnectFail_Attribute(CONNECT_FAIL_NONE, true, 3u) != CONNECT_ATTRIB_OK) {
        fail(tag, "CONNECT_FAIL_NONE must attribute as OK regardless of evidence");
    }
    EXPECT_TRUE(tag, strcmp(ConnectFail_AttributionText(CONNECT_ATTRIB_AMBIG_CONFIRM),
                            "ambiguous-punch-confirmed") == 0);
    EXPECT_TRUE(tag,
                strstr(ConnectFail_AttributionNote(CONNECT_ATTRIB_AMBIG_CONFIRM),
                       "CONFIRMED") != NULL);
    /* No note for the unambiguous verdicts — the extra line must not be
     * emitted when there is nothing to warn about. */
    EXPECT_TRUE(tag, ConnectFail_AttributionNote(CONNECT_ATTRIB_OK)[0] == '\0');
    EXPECT_TRUE(tag, ConnectFail_AttributionNote(CONNECT_ATTRIB_SUPPORTED)[0] == '\0');
    pass(tag, "the attribution helper refuses to blame NAT on a confirmed punch");
}

/* ======================================================================
 * Test 4 — the MT sink really lands worker-thread lines in the file
 * ====================================================================== */

#define OBS_MT_THREADS 4
#define OBS_MT_LINES   200

typedef struct {
    int thread_id;
} ObsMtArg;

static int obs_mt_thread(void* arg) {
    const ObsMtArg* a = (const ObsMtArg*)arg;
    for (int i = 0; i < OBS_MT_LINES; i++) {
        char line[128];
        SDL_snprintf(line, sizeof(line), "[netplay-mt-test] t=%d i=%03d",
                     a->thread_id, i);
        Netplay_LogConnectEventMT(line);
    }
    return 0;
}

static void test4_mt_sink(unsigned long long before_ts) {
    const char* tag = "test4-mt-sink";

    /* Main thread, before the workers — the happens-before the API
     * contract is built on. */
    Netplay_LogSinkInit();

    ObsMtArg args[OBS_MT_THREADS];
    SDL_Thread* tids[OBS_MT_THREADS];
    for (int t = 0; t < OBS_MT_THREADS; t++) {
        args[t].thread_id = t;
        tids[t] = SDL_CreateThread(obs_mt_thread, "obs_mt", &args[t]);
        if (tids[t] == NULL) {
            fail(tag, "SDL_CreateThread failed");
            for (int j = 0; j < t; j++) {
                SDL_WaitThread(tids[j], NULL);
            }
            return;
        }
    }
    for (int t = 0; t < OBS_MT_THREADS; t++) {
        SDL_WaitThread(tids[t], NULL);
    }

    /* The heartbeat must take the asynchronous mailbox path and be visible
     * after its explicit drain, without making this test run a live Gekko
     * session. This also proves the logger thread serialized its FILE write
     * before the file is read below. */
    Netplay_TestHook_HeartbeatEnqueue("[netplay-heartbeat-test] f=1");
    Netplay_TestHook_HeartbeatDrain();

    char path[768];
    if (!obs_find_new_log(before_ts, path, sizeof(path))) {
        fail(tag, "no new netplay-*.log was created by the MT sink");
        return;
    }
    size_t len = 0;
    char* body = obs_read_file(path, &len);
    if (body == NULL) {
        fail(tag, "could not read back the netplay log file");
        return;
    }

    EXPECT_TRUE(tag,
                strstr(body, "[netplay-heartbeat-test] f=1\n") != NULL);

    /* Every line must be present EXACTLY as written. Searching for the
     * whole line bounded by newlines is what catches a torn write: an
     * interleaved fputs would split a line and the bounded needle would
     * not match. */
    int missing = 0;
    int first_missing_t = -1, first_missing_i = -1;
    for (int t = 0; t < OBS_MT_THREADS; t++) {
        for (int i = 0; i < OBS_MT_LINES; i++) {
            char needle[160];
            SDL_snprintf(needle, sizeof(needle), "\n[netplay-mt-test] t=%d i=%03d\n",
                         t, i);
            /* The very first line of the file has no leading newline of
             * its own, so try the unanchored form too — only for the
             * file's opening line. */
            bool found = (strstr(body, needle) != NULL);
            if (!found) {
                SDL_snprintf(needle, sizeof(needle), "[netplay-mt-test] t=%d i=%03d\n",
                             t, i);
                found = (body == strstr(body, needle));
            }
            if (!found) {
                if (missing == 0) {
                    first_missing_t = t;
                    first_missing_i = i;
                }
                missing++;
            }
        }
    }
    if (missing != 0) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: %d of %d MT lines missing or "
                "torn in %s (first: t=%d i=%03d)\n",
                tag, missing, OBS_MT_THREADS * OBS_MT_LINES, path,
                first_missing_t, first_missing_i);
        fail_count++;
    } else {
        fprintf(stderr,
                "[test_connect_observability] PASS: %s: all %d lines from %d threads "
                "landed untorn in %s\n",
                tag, OBS_MT_THREADS * OBS_MT_LINES, OBS_MT_THREADS, path);
    }
    free(body);
}

/* ======================================================================
 * Test 5 — the S2 join retry does not inherit attempt 1's evidence (F1)
 * ====================================================================== */

static void test5_attempt_evidence_reset(void) {
    const char* tag = "test5-attempt-reset";

    /*
     * WHAT COULD NOT BE INDUCED, AND WHY.
     *
     * The finding is about the two-attempt path: join_thread_fn runs
     * join_attempt() a second time on the SAME s_work when attempt 1
     * returns a terminal failure, and s_work's whole-struct memsets fire
     * once per attempt SET, not per attempt. Driving that path in-process
     * was not possible here:
     *
     *   - The only entry point is DirectP2P_BeginJoin(), which requires a
     *     decodable room code and spawns join_thread_fn on its own
     *     thread. Attempt 1 then runs the real STUN_DISCOVER against the
     *     real public STUN server list, and the retry is reached ONLY
     *     after a terminal failure — so the fast path to two attempts is
     *     "have no network", which is not a condition a harness can
     *     assert on a developer machine or CI box that does have one.
     *   - With network, each attempt burns the real STUN budget plus the
     *     race budget before failing. Two attempts is tens of seconds of
     *     wall clock and an outcome that depends on the live rendezvous
     *     server's mood — a flaky test that also talks to production.
     *   - DirectP2P_TestHook_SetStunDiscover can stub discovery, but the
     *     retry decision, the terminal state and the socket lifecycle
     *     still run through a detached worker with no completion signal
     *     the harness can wait on, and s_work is file-static with no
     *     reader.
     *
     * So the assertion is made at the narrowest seam that is still the
     * REAL code: join_attempt()'s reset block, factored into
     * join_reset_attempt_evidence() and called through
     * DirectP2P_TestHook_JoinAttemptEvidenceReset. The hook seeds every
     * per-attempt field with a value attempt 1 could plausibly have left
     * behind, runs the production reset, and reports what survived. A
     * field added to Work and forgotten in the reset fails here exactly
     * as it would in the field.
     */
    DirectP2PAttemptEvidence in;
    memset(&in, 0, sizeof(in));
    /* Attempt 1: raced, saw the server, confirmed a punch, exhausted. */
    in.fail_code = (int)CONNECT_FAIL_NAT_BLOCKED;
    in.deliver_any = true;
    in.deliver_real = true;
    in.challenge_any = true;
    in.t_race_ms = 9123u;
    in.confirm_seen = true;
    in.confirm_ms = 7412u;
    in.deliver_n = 5u;
    in.challenge_n = 2u;
    in.badver_n = 3u;
    in.deliver_gap_max_ms = 1875u;

    DirectP2PAttemptEvidence out;
    memset(&out, 0xA5, sizeof(out)); /* poison: the hook must overwrite */
    DirectP2P_TestHook_JoinAttemptEvidenceReset(&in, &out);

    /* The pre-#36 fields — proof the seam really is the shipped block
     * and not an empty stub that trivially zeroes things. */
    EXPECT_TRUE(tag, out.fail_code == (int)CONNECT_FAIL_NONE);
    EXPECT_TRUE(tag, out.deliver_any == false);
    EXPECT_TRUE(tag, out.deliver_real == false);
    EXPECT_TRUE(tag, out.challenge_any == false);
    EXPECT_TRUE(tag, out.t_race_ms == 0u);

    /* THE FINDING: the six #36 fields, which the reset originally
     * missed. Reported individually — "one of them leaked" is not a
     * useful failure message when the fix is per-field. */
    struct { const char* name; unsigned long got; } leaked[6];
    int nleak = 0;
    if (out.confirm_seen) {
        leaked[nleak].name = "race_confirm_seen"; leaked[nleak].got = 1; nleak++;
    }
    if (out.confirm_ms != 0u) {
        leaked[nleak].name = "race_confirm_ms";
        leaked[nleak].got = (unsigned long)out.confirm_ms; nleak++;
    }
    if (out.deliver_n != 0u) {
        leaked[nleak].name = "ev_deliver_n";
        leaked[nleak].got = (unsigned long)out.deliver_n; nleak++;
    }
    if (out.challenge_n != 0u) {
        leaked[nleak].name = "ev_challenge_n";
        leaked[nleak].got = (unsigned long)out.challenge_n; nleak++;
    }
    if (out.badver_n != 0u) {
        leaked[nleak].name = "ev_badver_n";
        leaked[nleak].got = (unsigned long)out.badver_n; nleak++;
    }
    if (out.deliver_gap_max_ms != 0u) {
        leaked[nleak].name = "ev_deliver_gap_max_ms";
        leaked[nleak].got = (unsigned long)out.deliver_gap_max_ms; nleak++;
    }
    for (int i = 0; i < nleak; i++) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: %s survived the per-attempt "
                "reset (=%lu) — attempt 2's report would carry attempt 1's evidence\n",
                tag, leaked[i].name, leaked[i].got);
        fail_count++;
    }
    if (nleak == 0) {
        pass(tag, "all six #36 evidence fields are cleared per attempt");
    }

    /* And the concrete bad line the finding names: with the reset in
     * place, attempt 2 failing at STUN reports STUN_ALLDOWN with no
     * punch evidence attached, and the verdict is not softened by a
     * stale confirm. */
    const ConnectAttribution a2 =
        ConnectFail_Attribute(CONNECT_FAIL_STUN_ALLDOWN, out.confirm_seen,
                              out.badver_n);
    if (a2 != CONNECT_ATTRIB_SUPPORTED) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: a post-reset STUN_ALLDOWN "
                "attributed as \"%s\" — attempt 1's evidence is still in play\n",
                tag, ConnectFail_AttributionText(a2));
        fail_count++;
    } else {
        pass(tag, "attempt 2's STUN_ALLDOWN carries no punch evidence from attempt 1");
    }
}

/* ======================================================================
 * #44 shared helpers — scratch directories and file arithmetic
 * ====================================================================== */

/* A per-process scratch directory OUTSIDE PrefPath. The prune and the
 * report rotation are both destructive by design, and pointing them at
 * the real <PrefPath>logs/ to test them would mean deleting a developer's
 * (or a tester's) actual evidence to prove that deleting works. */
static void obs_tmp_dir(const char* what, char* out, size_t cap) {
    const char* base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    const size_t n = strlen(base);
    const char* sep = (n > 0 && base[n - 1] == '/') ? "" : "/";
    SDL_snprintf(out, cap, "%s%s3sx-obs-%s-%d", base, sep, what, (int)getpid());
}

static size_t obs_file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t n = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long end = ftell(f);
        if (end > 0) {
            n = (size_t)end;
        }
    }
    fclose(f);
    return n;
}

static bool obs_file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

static int obs_count_substr(const char* hay, const char* needle) {
    int n = 0;
    const size_t nlen = strlen(needle);
    if (nlen == 0) {
        return 0;
    }
    for (const char* p = strstr(hay, needle); p != NULL; p = strstr(p + nlen, needle)) {
        n++;
    }
    return n;
}

/* Delete every file this harness put in a scratch directory, then the
 * directory. Bounded, non-recursive, and it only ever removes plain
 * files it finds there — the same discipline the production prune uses. */
static void obs_rmtree_flat(const char* dir) {
    DIR* d = opendir(dir);
    if (d != NULL) {
        struct dirent* e;
        int examined = 0;
        while ((e = readdir(d)) != NULL && examined < 4096) {
            examined++;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            char path[1024];
            SDL_snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            remove(path);
        }
        closedir(d);
    }
    remove(dir);
}

/* ======================================================================
 * Test 9 — a MULTI-RUNG host failure is distinguishable from a one-rung
 *          one, in the tester report (#104)
 * ======================================================================
 *
 * WHAT WAS WRONG. The S2 host ladder re-enters host_thread_fn up to
 * 1 + HOST_STUN_MAX_RETRIES times, and every rung OVERWRITES t_upnp_ms /
 * t_stun_ms. The FAIL line's only attempt counter, `attempts=`, is
 * s_work.join_attempts — the JOINER's. So a host that burned four rungs
 * (and, per ORCH_HOST_WORST_CASE_MS, can legitimately consume the whole
 * derived nav deadline doing it: 4 port-map probes + 4 STUN budgets +
 * 3 x HOST_STUN_RETRY_BACKOFF_MS) emitted a line identical in every
 * field to a host that failed once, fast. Read literally, four slow
 * failures reported as one fast failure — the H-1 misattribution shape,
 * where bounded real work is discarded and the tester's network is
 * blamed for it.
 *
 * WHY THIS IS NOT A FORMATTER TEST. Both readings below come from
 * DRIVING THE REAL LADDER: DirectP2P_BeginHost spawns the real
 * host_thread_fn, which takes the real WORKER_STARTUP_DELAY_MS, runs the
 * real try_portmap, and calls the real STUN seam; the real
 * DirectP2P_Tick observes FAILED_STUN, waits the real backoff, joins the
 * dead worker and respawns. Only two things are dialled — the ladder's
 * LENGTH (so the same induced failure yields a four-rung and a one-rung
 * case to compare) and the backoff (so the suite does not sleep 15 s).
 * The STUN failure itself is genuine: the seam returns false with the
 * diagnostic counters a really-unanswered discovery leaves behind.
 *
 * The assertion is made TWICE over, on purpose. Once against the
 * counters (a formatter built from the wrong variables would still print
 * something), and once against the bytes actually in
 * netplay-report.txt — the file a tester sends us, which is the whole
 * subject of #104. A field that is right in memory and absent from the
 * report is not observability.
 *
 * ORDERING: before test6, which freezes the shared session log. */

/* A STUN seam that always fails, the way a really-unanswered discovery
 * does: servers probed, sends went out, nothing came back, and no
 * local-socket / CSPRNG fault (both of those would reclassify the
 * failure INTERNAL and are not what this test is about). The small
 * sleep gives every rung a non-zero, ACCUMULATING leg — a total that
 * stayed 0 would prove nothing about summation. */
#define OBS_LADDER_STUN_LEG_MS 25

static SDL_AtomicInt g_obs_ladder_stun_calls;

static bool obs_ladder_stun_always_fails(StunResult* result, uint16_t local_port,
                                         int timeout_ms) {
    (void)local_port;
    (void)timeout_ms;
    SDL_AddAtomicInt(&g_obs_ladder_stun_calls, 1);
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->diag_servers_probed = 3;
        result->diag_servers_answered = 0;
        result->diag_sends_ok = 3;
        result->diag_dns_all_failed = false;
        result->diag_socket_fail = false;
        result->diag_csprng_fail = false;
    }
    SDL_Delay(OBS_LADDER_STUN_LEG_MS);
    return false;
}

typedef struct {
    int      rungs;
    int      portmap_probes;
    uint32_t upnp_total_ms;
    uint32_t stun_total_ms;
    uint32_t backoff_ms;
    bool     reached_terminal;
    uint32_t wall_ms;
} ObsLadderRun;

/* Drive ONE hosting session to its terminal FAIL with the ladder dialled
 * to `max_retries`, and read back what the counters and the clock say.
 * The loop is the production main-loop shape — Tick once per frame —
 * with a wall-clock guard that is a liveness net, not an assertion. */
static void obs_drive_host_ladder(int max_retries, int backoff_ms, ObsLadderRun* out) {
    memset(out, 0, sizeof(*out));

    DirectP2P_TestHook_SetHostLadder(max_retries, backoff_ms);
    DirectP2P_TestHook_SetStunDiscover(obs_ladder_stun_always_fails);
    SDL_SetAtomicInt(&g_obs_ladder_stun_calls, 0);

    const uint64_t t0 = SDL_GetTicks();
    const uint64_t guard_ms = t0 + 30000u;

    DirectP2P_BeginHost(30717);
    while (SDL_GetTicks() < guard_ms) {
        DirectP2P_Tick();
        if (DirectP2P_GetState() == DIRECT_P2P_FAILED_STUN &&
            !DirectP2P_HostStunRetryPending()) {
            out->reached_terminal = true;
            break;
        }
        SDL_Delay(5);
    }
    out->wall_ms = (uint32_t)(SDL_GetTicks() - t0);

    DirectP2P_TestHook_HostLadderCounters(&out->rungs, &out->portmap_probes,
                                          &out->upnp_total_ms, &out->stun_total_ms,
                                          &out->backoff_ms);

    /* Put the module back to IDLE so the next drive starts clean, and
     * drop both seams. RunTeardown is the production teardown. */
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetHostLadder(-1, 0);
}

/* Pull the value of `key` out of a FAIL line, e.g. "host_rungs=" -> 4.
 * Returns -1 when the key is absent, which is the failure this test
 * exists to catch. */
static long obs_field_of(const char* line, const char* key) {
    const char* p = strstr(line, key);
    if (p == NULL) {
        return -1;
    }
    return strtol(p + strlen(key), NULL, 10);
}

/* The LAST "[netplay-connect] FAIL " line in the tester report. Copies
 * into `out` because the caller frees the body. */
static bool obs_last_fail_line(const char* body, char* out, size_t cap) {
    const char* k = "[netplay-connect] FAIL ";
    const char* best = NULL;
    for (const char* p = strstr(body, k); p != NULL; p = strstr(p + 1, k)) {
        best = p;
    }
    if (best == NULL) {
        return false;
    }
    const char* end = strchr(best, '\n');
    size_t n = (end != NULL) ? (size_t)(end - best) : strlen(best);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, best, n);
    out[n] = '\0';
    return true;
}

static void test9_host_ladder_rungs(const char* report_dir) {
    const char* tag = "test9-host-ladder-rungs";

    char report[768];
    SDL_snprintf(report, sizeof(report), "%s/netplay-report.txt", report_dir);

    /* Assert against the SHIPPED ladder, read from the module, so this
     * test starts failing if someone changes the constants without
     * revisiting the derivation in ORCH_HOST_WORST_CASE_MS. */
    int shipped_retries = -1;
    int shipped_backoff = -1;
    DirectP2P_TestHook_HostLadderLimits(&shipped_retries, &shipped_backoff);
    EXPECT_TRUE(tag, shipped_retries == 3);
    EXPECT_TRUE(tag, shipped_backoff == 5000);

    const int k_backoff = 60; /* real backoff, dialled down; see the note */

    /* ---- A: the full shipped-LENGTH ladder, every rung failing ---- */
    ObsLadderRun multi;
    obs_drive_host_ladder(shipped_retries, k_backoff, &multi);

    EXPECT_TRUE(tag, multi.reached_terminal);
    /* 1 initial attempt + shipped_retries retries, all of them real. */
    EXPECT_TRUE(tag, multi.rungs == 1 + shipped_retries);
    EXPECT_TRUE(tag, SDL_GetAtomicInt(&g_obs_ladder_stun_calls) == 1 + shipped_retries);
    /* #96's evidence field, and the regression test for #96 itself.
     *
     * No mapping is ever live here — the harness build refuses both SSDP
     * and gateway discovery — so this is exactly the FAILING-probe host
     * #96 is about. Before #96 the re-probe skip fired only for a LIVE
     * mapping, so every rung re-paid the probe and this read 4. With the
     * failure verdict latched for the hosting session it must read 1,
     * however long the ladder is: the shipped cost of a failing probe is
     * PORTMAP_PROBE_BUDGET_MS (measured: 8033 ms of miniupnpc 2.2.1 SSDP
     * plus 3510 ms of silent gateway overruns the 11250 ms outer
     * deadline), so each extra probe here would be 11.25 s of a real
     * user's time. Asserting == 1 rather than <= rungs is deliberate:
     * "fewer than before" is not the property, "once per session" is. */
    EXPECT_TRUE(tag, multi.portmap_probes == 1);
    EXPECT_TRUE(tag, multi.rungs > multi.portmap_probes);
    /* The summation is real, not a copy of the last rung. */
    EXPECT_TRUE(tag, multi.stun_total_ms >=
                         (uint32_t)(multi.rungs * OBS_LADDER_STUN_LEG_MS));
    /* Backoff is charged once per RETRY, not once per rung, and it is
     * measured rather than assumed — so it is at least the nominal sum.
     * Upper bound is generous: this runs on a loaded suite machine. */
    EXPECT_TRUE(tag, multi.backoff_ms >= (uint32_t)(shipped_retries * k_backoff));
    EXPECT_TRUE(tag, multi.backoff_ms <= (uint32_t)(shipped_retries * k_backoff) + 5000u);

    char multi_line[1400];
    bool have_multi = false;
    {
        size_t len = 0;
        char* body = obs_read_file(report, &len);
        if (body == NULL) {
            fail(tag, "the tester report does not exist after a host FAIL");
        } else {
            have_multi = obs_last_fail_line(body, multi_line, sizeof(multi_line));
            free(body);
        }
    }
    if (!have_multi) {
        fail(tag, "no [netplay-connect] FAIL line reached the tester report");
        return;
    }
    EXPECT_TRUE(tag, obs_field_of(multi_line, "host_rungs=") == 1 + shipped_retries);
    /* #96 in the tester report: four rungs, ONE probe. */
    EXPECT_TRUE(tag, obs_field_of(multi_line, "host_portmap_probes=") == 1);
    EXPECT_TRUE(tag, obs_field_of(multi_line, "t_ms_total upnp=") >= 0);
    EXPECT_TRUE(tag, obs_field_of(multi_line, "backoff=") == (long)multi.backoff_ms);
    /* The line must be COMPLETE — a truncated report line is a silently
     * degraded diagnostic, and these fields sit in the middle of it. */
    EXPECT_TRUE(tag, strstr(multi_line, "attrib=") != NULL);
    EXPECT_TRUE(tag, strstr(multi_line, "role=host") != NULL);

    /* ---- B: the SAME induced failure, one rung ---- */
    ObsLadderRun single;
    obs_drive_host_ladder(0, k_backoff, &single);

    EXPECT_TRUE(tag, single.reached_terminal);
    EXPECT_TRUE(tag, single.rungs == 1);
    EXPECT_TRUE(tag, single.portmap_probes == 1);
    EXPECT_TRUE(tag, single.backoff_ms == 0u); /* never retried, never waited */

    char single_line[1400];
    bool have_single = false;
    {
        size_t len = 0;
        char* body = obs_read_file(report, &len);
        if (body == NULL) {
            fail(tag, "the tester report vanished between the two drives");
        } else {
            have_single = obs_last_fail_line(body, single_line, sizeof(single_line));
            free(body);
        }
    }
    if (!have_single) {
        fail(tag, "the one-rung failure produced no tester-report line");
        return;
    }
    EXPECT_TRUE(tag, obs_field_of(single_line, "host_rungs=") == 1);
    EXPECT_TRUE(tag, obs_field_of(single_line, "host_portmap_probes=") == 1);
    EXPECT_TRUE(tag, obs_field_of(single_line, "backoff=") == 0);

    /* ---- THE POINT: the two are distinguishable ---- */
    /* Same code, same state, same role — that is what made the two
     * indistinguishable before #104, and it must STILL be true, or the
     * comparison below is proving something else. */
    {
        const char* a = strstr(multi_line, "code=");
        const char* b = strstr(single_line, "code=");
        EXPECT_TRUE(tag, a != NULL && b != NULL);
        if (a != NULL && b != NULL) {
            EXPECT_TRUE(tag, strncmp(a, b, 24) == 0);
        }
    }
    if (strcmp(multi_line, single_line) == 0) {
        fail(tag,
             "a four-rung host failure and a one-rung host failure produced "
             "identical report lines — #104 did not land");
        return;
    }
    /* And the multi-rung run really did cost more wall clock, so the
     * report is not merely different, it is TRUE. */
    EXPECT_TRUE(tag, multi.wall_ms > single.wall_ms);
    EXPECT_TRUE(tag, multi.stun_total_ms > single.stun_total_ms);

    if (fail_count == 0) {
        fprintf(stderr,
                "[test_connect_observability] PASS: %s: multi-rung host FAIL reports "
                "host_rungs=%d host_portmap_probes=%d backoff=%ums stun_total=%ums "
                "(wall %ums); one-rung reports host_rungs=%d host_portmap_probes=%d "
                "backoff=%ums stun_total=%ums (wall %ums) — distinguishable, and "
                "#96 keeps the probe count at 1 across the whole ladder\n",
                tag, multi.rungs, multi.portmap_probes, (unsigned)multi.backoff_ms,
                (unsigned)multi.stun_total_ms, (unsigned)multi.wall_ms,
                single.rungs, single.portmap_probes, (unsigned)single.backoff_ms,
                (unsigned)single.stun_total_ms, (unsigned)single.wall_ms);
    }
}

/* ======================================================================
 * Test 6 — the per-session byte budget actually stops the file growing
 * ======================================================================
 *
 * ORDERING NOTE, deliberate and load-bearing: this test EXHAUSTS the
 * budget of the one session log the whole run shares (netplay.c opens it
 * lazily on the first connect event and only closes it from a live
 * session's teardown, which this harness never reaches). Once the
 * TRUNCATED marker is published the file is frozen for the rest of the
 * process, so every test that reads the session log back — 1 through 4 —
 * must run BEFORE this one. That is not an accidental coupling: the
 * property under test is a process-global latch, and there is no honest
 * way to assert "the file stops growing" without stopping it.
 *
 * The lines are tagged [netplay-budget-test], NOT [netplay-connect], for
 * a second reason: they must not end up in the tester-facing report. Test
 * 8 relies on that same filter from the other direction. */
static void test6_byte_budget(unsigned long long before_ts) {
    const char* tag = "test6-byte-budget";

    char path[768];
    if (!obs_find_new_log(before_ts, path, sizeof(path))) {
        fail(tag, "no session log exists to test the budget against");
        return;
    }

    /* Pad each line so the budget is reached in a bounded number of
     * writes without depending on NETPLAY_LOG_MAX_BYTES being visible
     * here — it is private to netplay.c, and a test that hardcodes a
     * private constant silently stops testing anything the day it
     * changes. The loop cap is a liveness guard, not the assertion. */
    static const char k_pad[] =
        "................................................................"
        "................................................................"
        "................................................................"
        "...............................................................";
    const int   k_max_writes = 20000;
    const char* k_trunc = "[netplay-log] TRUNCATED at ";

    int  writes = 0;
    bool truncated = false;
    while (writes < k_max_writes) {
        char line[512];
        SDL_snprintf(line, sizeof(line), "[netplay-budget-test] n=%06d %s",
                     writes, k_pad);
        Netplay_LogConnectEventMT(line);
        writes++;
        if ((writes % 64) == 0) {
            size_t len = 0;
            char* body = obs_read_file(path, &len);
            if (body != NULL) {
                truncated = (strstr(body, k_trunc) != NULL);
                free(body);
            }
            if (truncated) {
                break;
            }
        }
    }

    if (!truncated) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: wrote %d padded lines and the "
                "session file never published a TRUNCATED marker — the budget is not "
                "bounding %s\n",
                tag, writes, path);
        fail_count++;
        return;
    }

    const size_t frozen_at = obs_file_size(path);

    /* Past the budget the sink must keep accepting lines (they still go
     * to SDL_Log) and must stop touching the disk. */
    for (int i = 0; i < 500; i++) {
        char line[512];
        SDL_snprintf(line, sizeof(line), "[netplay-budget-test] post=%06d %s",
                     i, k_pad);
        Netplay_LogConnectEventMT(line);
    }
    const size_t after = obs_file_size(path);

    if (after != frozen_at) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: session file grew from %lu to "
                "%lu bytes AFTER the TRUNCATED marker — the marker is a lie\n",
                tag, (unsigned long)frozen_at, (unsigned long)after);
        fail_count++;
    } else {
        fprintf(stderr,
                "[test_connect_observability] PASS: %s: session file froze at %lu bytes "
                "and stayed there across 500 further lines\n",
                tag, (unsigned long)frozen_at);
    }

    /* Exactly one marker. A per-line marker would be its own runaway. */
    size_t len = 0;
    char* body = obs_read_file(path, &len);
    if (body == NULL) {
        fail(tag, "could not read back the session log to count TRUNCATED markers");
        return;
    }
    const int markers = obs_count_substr(body, k_trunc);
    free(body);
    if (markers != 1) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: expected exactly 1 TRUNCATED "
                "marker, found %d\n",
                tag, markers);
        fail_count++;
    } else {
        pass(tag, "exactly one TRUNCATED marker was written");
    }
}

/* ======================================================================
 * Test 7 — the prune keeps the 20 newest and refuses everything else
 * ======================================================================
 *
 * The whole risk of #44's prune is that it deletes something it should
 * not. The two names it must never match are the very files the feature
 * exists to preserve, so both are planted in the directory alongside 30
 * genuine session logs and a bystander file. */
#define OBS_PRUNE_TOTAL 30
#define OBS_PRUNE_KEEP  20 /* mirrors NETPLAY_LOG_KEEP_FILES */

static bool obs_write_stub(const char* dir, const char* name, const char* body) {
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        return false;
    }
    fputs(body, f);
    fclose(f);
    return true;
}

static void test7_prune(void) {
    const char* tag = "test7-prune";

    char dir[512];
    obs_tmp_dir("prune", dir, sizeof(dir));
    obs_rmtree_flat(dir); /* a previous aborted run must not skew the count */
    if (!SDL_CreateDirectory(dir)) {
        fail(tag, "could not create the scratch directory");
        return;
    }

    /* Known, strictly increasing stamps so "the 20 newest" is a fact and
     * not a race against the filesystem clock. Deliberately NOT mtime
     * order — every file is created within the same millisecond, which is
     * exactly the case a stat()-based prune would get wrong. */
    const unsigned long long base_ts = 1700000000000ULL;
    for (int i = 0; i < OBS_PRUNE_TOTAL; i++) {
        char name[64];
        SDL_snprintf(name, sizeof(name), "netplay-%llu.log",
                     (unsigned long long)(base_ts + (unsigned long long)i * 1000ULL));
        if (!obs_write_stub(dir, name, "session\n")) {
            fail(tag, "could not create a stub session log");
            obs_rmtree_flat(dir);
            return;
        }
    }
    /* The three that must survive no matter what. */
    const bool planted =
        obs_write_stub(dir, "netplay-report.txt", "the one file a tester sends\n") &&
        obs_write_stub(dir, "netplay-report.1.txt", "the previous generation\n") &&
        obs_write_stub(dir, "keepme.txt", "an unrelated bystander\n");
    if (!planted) {
        fail(tag, "could not plant the must-not-delete files");
        obs_rmtree_flat(dir);
        return;
    }

    Netplay_TestHook_LogPrune(dir);

    /* Survivors, by construction: stamps base+10*1000 .. base+29*1000. */
    int wrong_deleted = 0;
    int wrong_kept = 0;
    unsigned long long first_wrong_deleted = 0;
    unsigned long long first_wrong_kept = 0;
    for (int i = 0; i < OBS_PRUNE_TOTAL; i++) {
        const unsigned long long ts = base_ts + (unsigned long long)i * 1000ULL;
        char path[1024];
        SDL_snprintf(path, sizeof(path), "%s/netplay-%llu.log", dir, ts);
        const bool present = obs_file_exists(path);
        const bool should_keep = (i >= OBS_PRUNE_TOTAL - OBS_PRUNE_KEEP);
        if (should_keep && !present) {
            if (wrong_deleted == 0) {
                first_wrong_deleted = ts;
            }
            wrong_deleted++;
        } else if (!should_keep && present) {
            if (wrong_kept == 0) {
                first_wrong_kept = ts;
            }
            wrong_kept++;
        }
    }
    if (wrong_deleted != 0) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: %d of the %d newest session logs "
                "were deleted (first: netplay-%llu.log)\n",
                tag, wrong_deleted, OBS_PRUNE_KEEP, first_wrong_deleted);
        fail_count++;
    }
    if (wrong_kept != 0) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: %d stale session logs survived the "
                "prune (first: netplay-%llu.log)\n",
                tag, wrong_kept, first_wrong_kept);
        fail_count++;
    }
    if (wrong_deleted == 0 && wrong_kept == 0) {
        fprintf(stderr,
                "[test_connect_observability] PASS: %s: exactly the %d newest of %d "
                "session logs survived\n",
                tag, OBS_PRUNE_KEEP, OBS_PRUNE_TOTAL);
    }

    /* THE assertion this test exists for. */
    static const char* k_must_survive[] = {
        "netplay-report.txt", "netplay-report.1.txt", "keepme.txt"
    };
    int lost = 0;
    for (int i = 0; i < 3; i++) {
        char path[1024];
        SDL_snprintf(path, sizeof(path), "%s/%s", dir, k_must_survive[i]);
        if (!obs_file_exists(path)) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: the prune deleted %s — the "
                    "filename filter is broken and it is eating the evidence\n",
                    tag, k_must_survive[i]);
            fail_count++;
            lost++;
        }
    }
    if (lost == 0) {
        pass(tag, "netplay-report.txt, netplay-report.1.txt and keepme.txt were untouched");
    }

    obs_rmtree_flat(dir);
}

/* ======================================================================
 * Test 8 — the report file rotates and stays pasteable
 * ======================================================================
 *
 * Two properties, and the second matters as much as the first: the file
 * is bounded (two generations, never unbounded growth on an SD card), and
 * it contains ONLY the attributed one-liners. A report that also carried
 * heartbeats would be neither small nor readable, and "send me this one
 * file" would stop being true. */
#define OBS_REPORT_MAX_BYTES (64u * 1024u) /* mirrors NETPLAY_REPORT_MAX_BYTES */

static void test8_report_rotation(const char* report_dir) {
    const char* tag = "test8-report-rotation";

    char live[768];
    char prev[768];
    SDL_snprintf(live, sizeof(live), "%s/netplay-report.txt", report_dir);
    SDL_snprintf(prev, sizeof(prev), "%s/netplay-report.1.txt", report_dir);

    /* Through the PRODUCTION entry point, not a test-only shim: this is
     * the same call direct_p2p.c's worker threads make, so the wiring
     * from a connect event to the report file is what is under test. */
    static const char k_pad[] =
        "----------------------------------------------------------------"
        "----------------------------------------------------------------";
    const int k_lines = 1500;
    for (int i = 0; i < k_lines; i++) {
        char line[512];
        SDL_snprintf(line, sizeof(line),
                     "[netplay-connect] FAIL code=P2P_FAIL_TEST n=%05d %s",
                     i, k_pad);
        Netplay_LogConnectEventMT(line);
    }
    /* The negative case, pushed through the identical door. */
    const char* k_excluded =
        "[netplay-heartbeat] rtt=12.3 frames_behind=0.4 — belongs in the session log";
    Netplay_LogConnectEventMT(k_excluded);

    const size_t live_n = obs_file_size(live);
    const size_t prev_n = obs_file_size(prev);

    if (prev_n == 0) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: %d lines produced no "
                "netplay-report.1.txt — the report never rotated and is unbounded\n",
                tag, k_lines);
        fail_count++;
    } else if (live_n == 0) {
        fail(tag, "netplay-report.txt is missing or empty after rotation");
    } else if (live_n >= (size_t)OBS_REPORT_MAX_BYTES) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: the live report is %lu bytes, at "
                "or past the %lu-byte cap — it did not restart\n",
                tag, (unsigned long)live_n, (unsigned long)OBS_REPORT_MAX_BYTES);
        fail_count++;
    } else {
        fprintf(stderr,
                "[test_connect_observability] PASS: %s: report rotated — live=%lu bytes, "
                "previous generation=%lu bytes\n",
                tag, (unsigned long)live_n, (unsigned long)prev_n);
    }

    /* Hard bound: two generations, each capped, plus at most the one line
     * that crossed the cap. */
    const size_t total = live_n + prev_n;
    const size_t bound = (size_t)OBS_REPORT_MAX_BYTES * 2u + 1024u;
    if (total >= bound) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: the two generations total %lu "
                "bytes, past the %lu-byte hard bound\n",
                tag, (unsigned long)total, (unsigned long)bound);
        fail_count++;
    } else {
        fprintf(stderr,
                "[test_connect_observability] PASS: %s: both generations total %lu bytes, "
                "inside the %lu-byte bound\n",
                tag, (unsigned long)total, (unsigned long)bound);
    }

    size_t len = 0;
    char* body = obs_read_file(live, &len);
    if (body == NULL) {
        fail(tag, "could not read back netplay-report.txt");
        return;
    }
    size_t prev_len = 0;
    char* prev_body = obs_read_file(prev, &prev_len);

    /* A fresh generation must carry its own header, otherwise a tester
     * who sends only the live file has no build or wire version. */
    EXPECT_TRUE(tag, strncmp(body, "=== 3S-ARM netplay report build=", 32) == 0);
    EXPECT_TRUE(tag, strstr(body, " rend_ver=") != NULL);
    EXPECT_TRUE(tag, strstr(body, " utc_ms=") != NULL);

    /* And the filter, from both sides. */
    const bool leaked =
        (strstr(body, "[netplay-heartbeat]") != NULL) ||
        (prev_body != NULL && strstr(prev_body, "[netplay-heartbeat]") != NULL);
    if (leaked) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: a non-[netplay-connect] line "
                "reached the report — the tester's one file is collecting noise\n",
                tag);
        fail_count++;
    } else {
        pass(tag, "a non-[netplay-connect] line was kept out of both generations");
    }
    /* Every body line is stamped. Without an absolute time this file
     * cannot be lined up against the session log or against what the
     * tester says they were doing, which is most of its value. */
    const char* hit = strstr(body, " [netplay-connect] FAIL code=P2P_FAIL_TEST");
    if (hit == NULL) {
        fail(tag, "the live report holds no [netplay-connect] line at all");
    } else if (hit == body || hit[-1] < '0' || hit[-1] > '9') {
        fail(tag, "a report line is not prefixed with a UTC-ms stamp");
    } else {
        pass(tag, "the live report holds stamped, attributed [netplay-connect] lines");
    }

    free(body);
    free(prev_body);
}

/* ====================================================================== */


/* ======================================================================
 * Test 10 — #122: a typed NACK, induced on a real socket, carried all
 *           the way to the VERDICT
 * ======================================================================
 *
 * THE HOLE THIS FILLS. eaf72865 shipped nine typed refusals and 36
 * server-side tests proving the server emits the right reason for the
 * right condition. Nothing on either side asserted that the CLIENT does
 * anything with them, and it did not: a valid v2 NACK matched no branch
 * in the joiner race loop and was destroyed with its datagram, so every
 * reason still collapsed into P2P_FAIL_RENDEZVOUS_DOWN — the pre-#122
 * behaviour, with the wire change deployed and unread.
 *
 * So the assertion here is NOT "the frame parses". It is "the frame
 * reaches the right verdict", per reason, over a real socket, through
 * the production classifier. The chain it completes:
 *
 *   server condition -> reason byte   __test_protocol.js testNackPerReason
 *   reason byte on the wire -> verdict  THIS TEST (end to end) and
 *                                       test_rendezvous_wire.c (pure)
 *
 * and the two halves are welded by the C-parity block inside
 * testNackPerReason, which reads the REND_NACK_* values straight out of
 * src/netplay/rendezvous.h. Neither side can renumber alone.
 *
 * Both negative controls are here too, because a NACK that says the
 * wrong thing is worse than silence (H-1) and the two gates that stop
 * one are exactly what a test must be able to break:
 *   10b — a NACK echoing a session key that is not ours (one bit off)
 *         must be ignored, and the verdict must fall back to
 *         RENDEZVOUS_DOWN.
 *   10c — a NACK from the right IP but the WRONG SOURCE PORT must be
 *         ignored for the same reason.
 */

/* Every reason, with the verdict it must produce. Hand-written, not
 * derived: a table generated from the mapping under test proves nothing.
 * The trailing row is a reason byte no build names — a server newer than
 * us — and must reach the honest catch-all rather than the nearest name
 * we happen to have. */
typedef struct {
    uint8_t         reason;
    const char*     name;
    ConnectFailCode want;
} ObsNackCase;

static const ObsNackCase k_obs_nack_cases[] = {
    { 1, "BAD_VERSION",  CONNECT_FAIL_RENDEZVOUS_BADFRAME },
    { 2, "BAD_LENGTH",   CONNECT_FAIL_RENDEZVOUS_BADFRAME },
    { 3, "BAD_TYPE",     CONNECT_FAIL_RENDEZVOUS_BADFRAME },
    { 4, "RATE_IP",      CONNECT_FAIL_RENDEZVOUS_BUSY },
    { 5, "RATE_KEY",     CONNECT_FAIL_RENDEZVOUS_BUSY },
    { 6, "RATE_PREGATE", CONNECT_FAIL_RENDEZVOUS_BUSY },
    { 7, "KEY_QUOTA",    CONNECT_FAIL_RENDEZVOUS_BUSY },
    { 8, "TABLE_FULL",   CONNECT_FAIL_RENDEZVOUS_TABLE_FULL },
    { 9, "SESSION_FULL", CONNECT_FAIL_RENDEZVOUS_ROOM_FULL },
    { 200, "unnamed",    CONNECT_FAIL_RENDEZVOUS_REFUSED },
};
#define OBS_NACK_CASES 10

/* The race only has to survive long enough for one REGISTER and one
 * reply; the mock answers the first datagram. Kept well under
 * OBS_RACE_BUDGET_MS because this test runs it twelve times. */
#define OBS_NACK_BUDGET_MS 450

/* Exactly what join_attempt() builds, from exactly the same fields, so a
 * verdict proved here is the verdict the joiner reports. */
static ConnectFailCode obs_verdict_for(const DirectP2PRaceProbeOut* out) {
    ConnectJoinEvidence ev;
    memset(&ev, 0, sizeof(ev));
    ev.deliver_any = false;   /* a signal-only race against a NACK-only mock */
    ev.deliver_real = false;
    ev.challenge_any = false;
    ev.cookie_rechallenged = false;
    ev.bilateral_punched = false;
    ev.port_disagreement = false;
    ev.punch_bad_token = false;
    ev.nack_any = out->nack_any;
    ev.nack_reason = out->nack_reason;
    return ConnectFail_ClassifyJoin(&ev);
}

/* Stand up a NACK mock, run one signal race against it, tear it down.
 * `alt` sends the reply from a second socket (the source-gate control). */
static bool obs_run_nack_race(uint8_t reason, bool bad_key, bool alt,
                              DirectP2PRaceProbeOut* out, int* out_replies) {
    unsigned short port = 0;
    const int sock = obs_open_udp_loopback(&port);
    if (sock < 0) {
        return false;
    }
    int alt_sock = 0;
    if (alt) {
        unsigned short alt_port = 0;
        alt_sock = obs_open_udp_loopback(&alt_port);
        if (alt_sock < 0) {
            close(sock);
            return false;
        }
    }
    ObsServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = sock;
    ctx.mode = OBS_REPLY_NACK;
    ctx.nack_reason = reason;
    ctx.nack_bad_key = bad_key;
    ctx.alt_sock = alt_sock;
    SDL_SetAtomicInt(&ctx.stop, 0);
    SDL_Thread* tid = SDL_CreateThread(obs_server_thread, "obs_nack", &ctx);
    if (tid == NULL) {
        close(sock);
        if (alt_sock > 0) close(alt_sock);
        return false;
    }

    memset(out, 0, sizeof(*out));
    const bool ran = obs_run_signal_race_ms(port, OBS_NACK_BUDGET_MS, out);

    SDL_SetAtomicInt(&ctx.stop, 1);
    SDL_WaitThread(tid, NULL);
    close(sock);
    if (alt_sock > 0) close(alt_sock);
    if (out_replies != NULL) {
        *out_replies = SDL_GetAtomicInt(&ctx.replies);
    }
    return ran;
}

static void test10_nack_per_reason(void) {
    const char* tag = "test10-nack-per-reason";
    int cases = 0;

    /* --- 10a. every reason, induced, carried to its verdict ---------- */
    for (int i = 0; i < OBS_NACK_CASES; i++) {
        const ObsNackCase* c = &k_obs_nack_cases[i];
        DirectP2PRaceProbeOut out;
        int replies = 0;
        if (!obs_run_nack_race(c->reason, false, false, &out, &replies)) {
            fail(tag, "could not stand up the NACK mock");
            return;
        }
        if (replies < 1) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: %s: the mock sent no "
                    "reply — the race never REGISTERed\n", tag, c->name);
            fail_count++;
            continue;
        }
        if (!out.nack_any) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: %s: reason %u was on the "
                    "wire (%d replies) and the race recorded NO nack — this is the "
                    "#122 defect\n",
                    tag, c->name, (unsigned)c->reason, replies);
            fail_count++;
            continue;
        }
        if (out.nack_reason != c->reason) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: %s: recorded reason %u, "
                    "sent %u\n",
                    tag, c->name, (unsigned)out.nack_reason, (unsigned)c->reason);
            fail_count++;
        }
        EXPECT_TRUE(tag, out.nack_n >= 1);
        /* A NACK is NOT a DELIVER and must not be miscounted as one:
         * deliver_any drives a completely different arm of the taxonomy. */
        EXPECT_TRUE(tag, out.deliver_n == 0);
        EXPECT_TRUE(tag, out.challenge_n == 0);
        /* ...and it is not a version skew either. The frame carries OUR
         * version, so badver_n must stay 0 — otherwise the report would
         * claim a definite version mismatch on top of the refusal. */
        EXPECT_TRUE(tag, out.badver_n == 0);

        const ConnectFailCode got = obs_verdict_for(&out);
        if (got != c->want) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: %s: verdict %s, want %s\n",
                    tag, c->name, ConnectFail_Code(got), ConnectFail_Code(c->want));
            fail_count++;
        }
        if (got == CONNECT_FAIL_RENDEZVOUS_DOWN) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: %s: still collapsed to "
                    "RENDEZVOUS_DOWN\n", tag, c->name);
            fail_count++;
        }
        cases++;
    }
    /* SWEEP FLOOR: a table row that stops running must fail the test, not
     * shrink it. */
    if (cases != OBS_NACK_CASES) {
        fprintf(stderr,
                "[test_connect_observability] FAIL: %s: ran %d of %d reason cases\n",
                tag, cases, OBS_NACK_CASES);
        fail_count++;
    } else {
        pass(tag, "all ten induced reasons reached their own verdict");
    }

    /* --- 10b. NEGATIVE: a NACK echoing someone else's session key ----
     * The key gate is what stops an off-path sender choosing our
     * diagnosis. One bit is enough to make the frame not ours. */
    {
        DirectP2PRaceProbeOut out;
        int replies = 0;
        if (!obs_run_nack_race(9 /* SESSION_FULL */, true, false, &out, &replies)) {
            fail(tag, "could not stand up the wrong-key NACK mock");
            return;
        }
        EXPECT_TRUE(tag, replies > 0);
        if (out.nack_any) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: a NACK carrying a "
                    "one-bit-wrong session key was ACCEPTED (reason=%u)\n",
                    tag, (unsigned)out.nack_reason);
            fail_count++;
        } else {
            const ConnectFailCode got = obs_verdict_for(&out);
            if (got != CONNECT_FAIL_RENDEZVOUS_DOWN) {
                fprintf(stderr,
                        "[test_connect_observability] FAIL: %s: an ignored NACK must "
                        "leave the verdict at RENDEZVOUS_DOWN, got %s\n",
                        tag, ConnectFail_Code(got));
                fail_count++;
            } else {
                pass(tag, "a wrong-key NACK is ignored and the verdict falls back");
            }
        }
    }

    /* --- 10c. NEGATIVE: a NACK from the wrong SOURCE PORT ------------
     * Right host, right key, wrong port: not the endpoint this signal leg
     * is REGISTERing to, so it is not the server and its diagnosis is not
     * evidence. This is the badver source gate (F5) applied to a frame
     * that decides a VERDICT rather than merely a counter. */
    {
        DirectP2PRaceProbeOut out;
        int replies = 0;
        if (!obs_run_nack_race(8 /* TABLE_FULL */, false, true, &out, &replies)) {
            fail(tag, "could not stand up the alt-port NACK mock");
            return;
        }
        EXPECT_TRUE(tag, replies > 0);
        if (out.nack_any) {
            fprintf(stderr,
                    "[test_connect_observability] FAIL: %s: a NACK from a source port "
                    "that is not the rendezvous endpoint was ACCEPTED (reason=%u)\n",
                    tag, (unsigned)out.nack_reason);
            fail_count++;
        } else {
            pass(tag, "a NACK from the wrong source port is ignored");
        }
    }
}

int Netplay_Test_ConnectObservability(void) {
    fail_count = 0;

    if (!SDL_Init(0)) {
        fprintf(stderr, "[test_connect_observability] SDL_Init failed: %s\n",
                SDL_GetError());
        return 1;
    }
    NET_Init();

    /* #44: redirect the tester-facing report at a scratch directory for
     * the WHOLE run, before any test can emit a connect event. Otherwise
     * this harness would append to — and test8 would rotate and then
     * delete — the real <PrefPath>logs/netplay-report.txt, i.e. the one
     * artifact the feature exists to hand a tester. */
    char report_dir[512];
    obs_tmp_dir("report", report_dir, sizeof(report_dir));
    obs_rmtree_flat(report_dir); /* no state from an aborted earlier run */
    SDL_CreateDirectory(report_dir);
    Netplay_TestHook_ReportDir(report_dir);

    /* Snapshot BEFORE anything can open a log, so the file this run
     * creates is identifiable and nothing pre-existing is ever touched. */
    const unsigned long long before_ts = obs_newest_log_ts();

    test1_version_skew(before_ts);
    test2_silent_is_ambiguous();
    test3_confirm_not_nat();
    test4_mt_sink(before_ts);
    test5_attempt_evidence_reset();
    /* #122: induced NACKs on a real socket. Runs before the log-file
     * tests for the same reason test5 does — it neither reads nor
     * freezes the shared session log. */
    test10_nack_per_reason();
    /* #104: drives the real host ladder and reads the tester report
     * back. Before test6 (which freezes the shared session log) and
     * before test8 (which floods and rotates the tester report). */
    test9_host_ladder_rungs(report_dir);
    /* test6 freezes the shared session log; 1-4 read it back, so it runs
     * after them by construction. See the note above test6. */
    test6_byte_budget(before_ts);
    test7_prune();
    test8_report_rotation(report_dir);

    /* Clean up ONLY the file this run created. */
    char path[768];
    if (obs_find_new_log(before_ts, path, sizeof(path))) {
        Netplay_FlushDiagnostics(); /* closes the sink's FILE* */
        if (remove(path) != 0) {
            fprintf(stderr,
                    "[test_connect_observability] note: could not remove %s\n", path);
        }
    }

    /* #44: drop the redirect and take the scratch report with it. The
     * order matters — the FILE* must be closed (which the redirect reset
     * does) before the directory goes away. */
    Netplay_TestHook_ReportDir(NULL);
    obs_rmtree_flat(report_dir);
    Netplay_LogSinkShutdown();

    if (fail_count > 0) {
        fprintf(stderr, "[test_connect_observability] %d failure(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "[test_connect_observability] OK — all cases passed\n");
    return 0;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_ConnectObservability(void) {
    fprintf(stderr,
            "[test_connect_observability] not compiled in; rebuild with "
            "-DENABLE_NETPLAY_TESTS to enable.\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
