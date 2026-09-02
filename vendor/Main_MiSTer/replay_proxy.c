/*
 * replay_proxy.c — Stage S3a of docs/plan-osd-replay-browser.md.
 *
 * De-SDL'd port of src/replay/proxy_client.c for the HPS OSD wrapper. See
 * replay_proxy.h for the API contract, the de-SDL map, and the atomics choice.
 * The framing / JSON / base64 / manifest-loop / async logic below is a faithful
 * copy of the in-game client with SDL swapped for libc + POSIX + pthread.
 */

#include "replay_proxy.h"

#include "cJSON.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- Tunables (identical to proxy_client.c) ----------------------------- */

#define RP_CONNECT_TIMEOUT_S 5      /* connect budget */
#define RP_RESPONSE_TIMEOUT_MS 8000 /* read budget for the reply frame */
/* A search response with 50 rows is a few KiB; anything past this is garbage or
 * a hostile length. Refuse a multi-MB claim before allocating for it. */
#define RP_MAX_FRAME_LEN (256u * 1024u)

/* Canonical on-device game config path (kRuntimeHome/config in the wrapper). */
#define RP_CONFIG_PATH "/media/fat/games/3s-arm/config"

/* ---- libc string helper (strlcpy without a glibc-2.38 dependency, matching
 * replay_scan.c's rb_strlcpy) ---------------------------------------------- */

static size_t rp_strlcpy(char* dst, const char* src, size_t dst_sz) {
    size_t i = 0;
    if (dst_sz == 0) {
        return strlen(src);
    }
    for (; src[i] != '\0' && i + 1 < dst_sz; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return strlen(src);
}

/* ---- Byte-wise framing (ARM-safe; mirrors fcade_stream.c) --------------- */

static void put_u32be(uint8_t* out, uint32_t v) {
    out[0] = (uint8_t)((v >> 24) & 0xFF);
    out[1] = (uint8_t)((v >> 16) & 0xFF);
    out[2] = (uint8_t)((v >> 8) & 0xFF);
    out[3] = (uint8_t)(v & 0xFF);
}

static uint32_t get_u32be(const uint8_t* in) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | ((uint32_t)in[3]);
}

/* ---- Async state: pthread_mutex-guarded int (no C11 _Atomic under gnu99) - */

typedef struct RpAtomic {
    pthread_mutex_t lock;
    int state;
} RpAtomic;

static int atomic_get(RpAtomic* a) {
    pthread_mutex_lock(&a->lock);
    int v = a->state;
    pthread_mutex_unlock(&a->lock);
    return v;
}

static void atomic_set(RpAtomic* a, int v) {
    pthread_mutex_lock(&a->lock);
    a->state = v;
    pthread_mutex_unlock(&a->lock);
}

/* ---- Base64 decode (standard alphabet, RFC 4648 §4) ---------------------
 * Device only ever DECODES (the proxy encodes). Ignores whitespace defensively;
 * rejects invalid characters and an unpadded single-digit tail. */

static int b64_char_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1; /* '=' padding, whitespace, or invalid */
}

/* Decodes `in` (NUL-terminated base64 text) into a freshly malloc'd buffer.
 * Returns the buffer and sets *out_len on success (caller frees it); returns
 * NULL on any malformed input. An empty input decodes to a valid zero-length
 * buffer (*out_len == 0, never dereferenced by the caller). */
static uint8_t* b64_decode(const char* in, size_t* out_len) {
    size_t in_len = strlen(in);
    /* Upper bound: every 4 input chars decode to at most 3 output bytes. */
    uint8_t* out = (uint8_t*)malloc(in_len / 4 * 3 + 3);
    if (out == NULL)
        return NULL;

    size_t out_pos = 0;
    int group[4];
    int group_n = 0;

    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        int v = b64_char_value(c);
        if (v < 0) {
            free(out);
            return NULL; /* invalid character */
        }
        group[group_n++] = v;
        if (group_n == 4) {
            out[out_pos++] = (uint8_t)((group[0] << 2) | (group[1] >> 4));
            out[out_pos++] = (uint8_t)((group[1] << 4) | (group[2] >> 2));
            out[out_pos++] = (uint8_t)((group[2] << 6) | group[3]);
            group_n = 0;
        }
    }

    /* Leftover partial group (the padding-stripped tail of the last quartet). */
    if (group_n == 2) {
        out[out_pos++] = (uint8_t)((group[0] << 2) | (group[1] >> 4));
    } else if (group_n == 3) {
        out[out_pos++] = (uint8_t)((group[0] << 2) | (group[1] >> 4));
        out[out_pos++] = (uint8_t)((group[1] << 4) | (group[2] >> 2));
    } else if (group_n == 1) {
        free(out);
        return NULL; /* a single leftover base64 digit can't decode to a byte */
    }

    *out_len = out_pos;
    return out;
}

/* ---- Socket helpers ----------------------------------------------------- */

static bool send_all(int fd, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char*)buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

/* Read exactly `size` bytes into `out`. Returns true on success, false on
 * timeout (SO_RCVTIMEO), EOF, or error. */
static bool recv_exact(int fd, uint8_t* out, size_t size) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = recv(fd, (char*)out + got, size - got, 0);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0)
            return false; /* EOF */
        if (errno == EINTR)
            continue;
        return false; /* timeout (EAGAIN/EWOULDBLOCK) or hard error */
    }
    return true;
}

/* Non-blocking connect bounded by select(), like fcade_stream.c. Returns a
 * connected fd or -1. */
static int connect_with_timeout(struct addrinfo* ai, int timeout_s) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
        return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
        fcntl(fd, F_SETFL, flags);
        return fd;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv;
    tv.tv_sec = timeout_s;
    tv.tv_usec = 0;
    rc = select(fd + 1, NULL, &wset, NULL, &tv);
    if (rc <= 0) {
        close(fd);
        return -1;
    }

    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
        close(fd);
        return -1;
    }

    fcntl(fd, F_SETFL, flags); /* back to blocking (SO_RCVTIMEO bounds reads) */
    return fd;
}

/* ---- Response parse ----------------------------------------------------- */

/* Map the proxy's typed `error` string to our enum. Unknown strings fall to
 * RP_ERR_PROTOCOL. */
static RpError error_from_string(const char* s) {
    if (s == NULL)
        return RP_ERR_PROTOCOL;
    if (strcmp(s, "cookie_missing") == 0)
        return RP_ERR_COOKIE_MISSING;
    if (strcmp(s, "cloudflare_403") == 0)
        return RP_ERR_CLOUDFLARE_403;
    if (strcmp(s, "upstream_error") == 0)
        return RP_ERR_UPSTREAM;
    if (strcmp(s, "bad_request") == 0)
        return RP_ERR_BAD_REQUEST;
    if (strcmp(s, "not_found") == 0)
        return RP_ERR_NOT_FOUND;
    return RP_ERR_PROTOCOL;
}

static void copy_str_field(const cJSON* obj, const char* key, char* dst, size_t dst_sz) {
    dst[0] = '\0';
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        rp_strlcpy(dst, v->valuestring, dst_sz);
    }
}

static long long number_field(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        return (long long)v->valuedouble;
    }
    return 0;
}

static void parse_player(const cJSON* players, int idx, char* dst, size_t dst_sz, int* rank_out) {
    dst[0] = '\0';
    *rank_out = 0;
    const cJSON* item = cJSON_GetArrayItem(players, idx);
    if (item == NULL)
        return;
    /* Each player is {name,country,rank,score}. Accept a bare string too. */
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        rp_strlcpy(dst, item->valuestring, dst_sz);
        return;
    }
    if (cJSON_IsObject(item)) {
        copy_str_field(item, "name", dst, dst_sz);
        /* Fightcade rank 1..6 (E..S); 0/absent/out-of-range = unranked.
         * Accept a number or a numeric string (normalizePlayer passes either
         * through). Never fabricate: anything unrecognized stays 0. */
        const cJSON* rank = cJSON_GetObjectItemCaseSensitive(item, "rank");
        long long v = 0;
        if (cJSON_IsNumber(rank)) {
            v = (long long)rank->valuedouble;
        } else if (cJSON_IsString(rank) && rank->valuestring != NULL) {
            v = atoll(rank->valuestring);
        }
        if (v >= 1 && v <= 6) {
            *rank_out = (int)v;
        }
    }
}

/* Parse a full search response frame body. Fills *out (already zeroed). */
static void parse_response(const char* text, size_t len, RpSearchResults* out) {
    cJSON* root = cJSON_ParseWithLength(text, len);
    if (root == NULL) {
        out->error = RP_ERR_PROTOCOL;
        rp_strlcpy(out->detail, "unparseable response JSON", sizeof(out->detail));
        return;
    }

    const cJSON* ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok)) {
        out->error = RP_ERR_PROTOCOL;
        rp_strlcpy(out->detail, "response missing boolean 'ok'", sizeof(out->detail));
        cJSON_Delete(root);
        return;
    }

    if (!cJSON_IsTrue(ok)) {
        const cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
        out->error = error_from_string(cJSON_IsString(err) ? err->valuestring : NULL);
        copy_str_field(root, "detail", out->detail, sizeof(out->detail));
        cJSON_Delete(root);
        return;
    }

    /* ok:true — parse rows. */
    const cJSON* rows = cJSON_GetObjectItemCaseSensitive(root, "rows");
    if (!cJSON_IsArray(rows)) {
        out->error = RP_ERR_PROTOCOL;
        rp_strlcpy(out->detail, "ok response missing 'rows' array", sizeof(out->detail));
        cJSON_Delete(root);
        return;
    }

    int n = 0;
    const cJSON* item = NULL;
    cJSON_ArrayForEach(item, rows) {
        if (n >= RP_MAX_ROWS)
            break;
        if (!cJSON_IsObject(item))
            continue;
        RpRow* r = &out->rows[n];
        copy_str_field(item, "quarkid", r->quarkid, sizeof(r->quarkid));
        copy_str_field(item, "emulator", r->emulator, sizeof(r->emulator));
        copy_str_field(item, "gameid", r->gameid, sizeof(r->gameid));
        r->date_ms = number_field(item, "date");
        r->duration_secs = number_field(item, "duration");
        r->num_matches = (int)number_field(item, "num_matches");
        const cJSON* ranked = cJSON_GetObjectItemCaseSensitive(item, "ranked");
        r->ranked = cJSON_IsTrue(ranked);
        const cJSON* players = cJSON_GetObjectItemCaseSensitive(item, "players");
        if (cJSON_IsArray(players)) {
            parse_player(players, 0, r->p1, sizeof(r->p1), &r->p1_rank);
            parse_player(players, 1, r->p2, sizeof(r->p2), &r->p2_rank);
        }
        /* A row with no quarkid is useless (cannot be fetched) — skip it. */
        if (r->quarkid[0] == '\0')
            continue;
        n++;
    }

    out->count = n;
    out->error = RP_ERR_NONE;
    cJSON_Delete(root);
}

/* ---- Shared wire round-trip (connect + one framed request/response) ------
 * On success `*out_body` is a freshly malloc'd NUL-terminated buffer of
 * `*out_body_len` bytes (NUL not counted); the caller frees it. On failure
 * `*err`/`detail` are set to the client-local reason (RP_ERR_CONNECT or
 * RP_ERR_PROTOCOL only — the proxy's typed errors are decided later by parsing
 * the response body's ok/error fields). */
static int proxy_wire_roundtrip(const char* host, int port, const char* json, size_t json_len, char** out_body,
                                 size_t* out_body_len, RpError* err, char* detail, size_t detail_sz) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* IPv4 only — MiSTer stack precedent */
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || res == NULL) {
        *err = RP_ERR_CONNECT;
        snprintf(detail, detail_sz, "DNS failed for %s: %s", host, gai_strerror(gai));
        if (res)
            freeaddrinfo(res);
        return 1;
    }

    int fd = -1;
    for (struct addrinfo* ai = res; ai != NULL; ai = ai->ai_next) {
        fd = connect_with_timeout(ai, RP_CONNECT_TIMEOUT_S);
        if (fd >= 0)
            break;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        *err = RP_ERR_CONNECT;
        snprintf(detail, detail_sz, "connect to %s:%d failed", host, port);
        return 1;
    }

    struct timeval rtv;
    rtv.tv_sec = RP_RESPONSE_TIMEOUT_MS / 1000;
    rtv.tv_usec = (RP_RESPONSE_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    /* Send the framed request. */
    uint8_t lenbe[4];
    put_u32be(lenbe, (uint32_t)json_len);
    bool sent = send_all(fd, lenbe, 4) && send_all(fd, (const uint8_t*)json, json_len);
    if (!sent) {
        *err = RP_ERR_CONNECT;
        rp_strlcpy(detail, "send failed", detail_sz);
        close(fd);
        return 1;
    }

    /* Read one framed response. */
    uint8_t rlenbe[4];
    if (!recv_exact(fd, rlenbe, 4)) {
        *err = RP_ERR_CONNECT;
        rp_strlcpy(detail, "no/short response (timeout or closed)", detail_sz);
        close(fd);
        return 1;
    }
    uint32_t rlen = get_u32be(rlenbe);
    if (rlen == 0 || rlen > RP_MAX_FRAME_LEN) {
        *err = RP_ERR_PROTOCOL;
        snprintf(detail, detail_sz, "bad response frame length %u", rlen);
        close(fd);
        return 1;
    }

    char* body = (char*)malloc(rlen + 1);
    if (body == NULL) {
        *err = RP_ERR_PROTOCOL;
        close(fd);
        return 1;
    }
    if (!recv_exact(fd, (uint8_t*)body, rlen)) {
        *err = RP_ERR_CONNECT;
        rp_strlcpy(detail, "truncated response body", detail_sz);
        free(body);
        close(fd);
        return 1;
    }
    body[rlen] = '\0';
    close(fd);

    *out_body = body;
    *out_body_len = rlen;
    return 0;
}

/* ---- Synchronous search ------------------------------------------------- */

int RpSearch(const RpSearchParams* params, RpSearchResults* out) {
    memset(out, 0, sizeof(*out));
    out->error = RP_ERR_PROTOCOL;

    if (params == NULL || params->host[0] == '\0') {
        out->error = RP_ERR_DISABLED;
        rp_strlcpy(out->detail, "no replay-proxy-host configured", sizeof(out->detail));
        return 1;
    }

    cJSON* req = cJSON_CreateObject();
    if (req == NULL) {
        out->error = RP_ERR_PROTOCOL;
        return 1;
    }
    cJSON_AddStringToObject(req, "op", "search");
    cJSON_AddStringToObject(req, "gameid", params->gameid);
    cJSON_AddNumberToObject(req, "offset", params->offset < 0 ? 0 : params->offset);
    int limit = params->limit;
    if (limit < 1)
        limit = 1;
    if (limit > RP_MAX_ROWS)
        limit = RP_MAX_ROWS;
    cJSON_AddNumberToObject(req, "limit", limit);
    if (params->best)
        cJSON_AddBoolToObject(req, "best", true);
    if (params->since > 0)
        cJSON_AddNumberToObject(req, "since", (double)params->since);
    if (params->username[0] != '\0')
        cJSON_AddStringToObject(req, "username", params->username);

    char* json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (json == NULL) {
        out->error = RP_ERR_PROTOCOL;
        return 1;
    }

    const size_t json_len = strlen(json);

    char* body = NULL;
    size_t body_len = 0;
    if (proxy_wire_roundtrip(params->host, params->port, json, json_len, &body, &body_len, &out->error, out->detail,
                              sizeof(out->detail)) != 0) {
        free(json);
        return 1;
    }
    free(json);

    parse_response(body, body_len, out);
    free(body);

    return out->error == RP_ERR_NONE ? 0 : 1;
}

/* ---- Async search (single in-flight, polled state) ---------------------- */

static pthread_t s_thread;
static bool s_thread_valid = false;
static RpAtomic s_async_state = { PTHREAD_MUTEX_INITIALIZER, RP_ASYNC_IDLE };
static RpSearchParams s_async_params;
static RpSearchResults s_async_results;

static void* proxy_worker(void* unused) {
    (void)unused;
    RpSearch(&s_async_params, &s_async_results);
    /* Publish results before the state flips to DONE so a poller that observes
     * DONE always sees a fully-written results struct. */
    atomic_set(&s_async_state, RP_ASYNC_DONE);
    return NULL;
}

bool RpSearchAsync(const RpSearchParams* params) {
    if (params == NULL)
        return false;
    if (atomic_get(&s_async_state) != RP_ASYNC_IDLE)
        return false;

    /* Reap a previous finished-but-joined thread handle if any (defensive). */
    if (s_thread_valid) {
        pthread_join(s_thread, NULL);
        s_thread_valid = false;
    }

    s_async_params = *params;
    memset(&s_async_results, 0, sizeof(s_async_results));
    atomic_set(&s_async_state, RP_ASYNC_RUNNING);

    if (pthread_create(&s_thread, NULL, proxy_worker, NULL) != 0) {
        atomic_set(&s_async_state, RP_ASYNC_IDLE);
        fprintf(stderr, "replay_proxy: failed to spawn search thread\n");
        return false;
    }
    s_thread_valid = true;
    return true;
}

RpAsyncState RpSearchPoll(void) {
    return (RpAsyncState)atomic_get(&s_async_state);
}

bool RpSearchTake(RpSearchResults* out) {
    if (atomic_get(&s_async_state) != RP_ASYNC_DONE)
        return false;

    if (s_thread_valid) {
        pthread_join(s_thread, NULL);
        s_thread_valid = false;
    }
    if (out != NULL)
        *out = s_async_results;
    atomic_set(&s_async_state, RP_ASYNC_IDLE);
    return true;
}

void RpSearchCancel(void) {
    /* No socket-level cancel hook; the search is bounded by its own timeouts.
     * Join whatever is in flight so we never leak the thread. */
    if (s_thread_valid) {
        pthread_join(s_thread, NULL);
        s_thread_valid = false;
    }
    atomic_set(&s_async_state, RP_ASYNC_IDLE);
}

/* ---------------------------------------------------------------------- */
/* get3sr: fetch a pre-converted .3sr + .meta.json (Stage S1)              */
/* ---------------------------------------------------------------------- */

/* Recursive `mkdir -p` (mirrors fcade_stream.c mkdir_p). Returns 0 on success
 * (or already-exists). */
static int mkdir_p(const char* path) {
    char buf[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf))
        return -1;
    memcpy(buf, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST)
                return -1;
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0777) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Writes `data` (`len` bytes) to `path`. Returns true on success. */
static bool write_file_bytes(const char* path, const uint8_t* data, size_t len) {
    FILE* fp = fopen(path, "wb");
    if (fp == NULL)
        return false;
    bool ok = (len == 0) || (fwrite(data, 1, len, fp) == len);
    fclose(fp);
    return ok;
}

/* Base64-decodes one game object ({"game_index","b64","meta_b64",...}) and
 * writes BOTH decoded blobs under replay_root/quarkid/game_N.{3sr,meta.json}.
 * Writing the .meta.json sidecar is NOT optional — a decode/write failure on
 * EITHER half fails the whole game entry, never a half-written pair. */
static bool fetch3sr_write_game(const cJSON* item, const char* replay_root, const char* quarkid, Rp3srGame* game_out,
                                 char* detail, size_t detail_sz) {
    const cJSON* game_index_j = cJSON_GetObjectItemCaseSensitive(item, "game_index");
    const cJSON* b64_j = cJSON_GetObjectItemCaseSensitive(item, "b64");
    const cJSON* meta_b64_j = cJSON_GetObjectItemCaseSensitive(item, "meta_b64");

    if (!cJSON_IsNumber(game_index_j) || !cJSON_IsString(b64_j) || b64_j->valuestring == NULL ||
        !cJSON_IsString(meta_b64_j) || meta_b64_j->valuestring == NULL) {
        rp_strlcpy(detail, "game entry missing game_index/b64/meta_b64", detail_sz);
        return false;
    }

    int game_index = (int)game_index_j->valuedouble;

    size_t sr_len = 0;
    uint8_t* sr_bytes = b64_decode(b64_j->valuestring, &sr_len);
    if (sr_bytes == NULL) {
        snprintf(detail, detail_sz, "game_%d: b64 did not decode", game_index);
        return false;
    }

    size_t meta_len = 0;
    uint8_t* meta_bytes = b64_decode(meta_b64_j->valuestring, &meta_len);
    if (meta_bytes == NULL) {
        free(sr_bytes);
        snprintf(detail, detail_sz, "game_%d: meta_b64 did not decode", game_index);
        return false;
    }

    char quark_dir[400];
    snprintf(quark_dir, sizeof(quark_dir), "%s/%s", replay_root, quarkid);
    if (mkdir_p(quark_dir) != 0) {
        free(sr_bytes);
        free(meta_bytes);
        snprintf(detail, detail_sz, "game_%d: could not create %s", game_index, quark_dir);
        return false;
    }

    char path_3sr[512];
    char path_meta[512];
    snprintf(path_3sr, sizeof(path_3sr), "%s/game_%d.3sr", quark_dir, game_index);
    snprintf(path_meta, sizeof(path_meta), "%s/game_%d.meta.json", quark_dir, game_index);

    bool ok = write_file_bytes(path_3sr, sr_bytes, sr_len) && write_file_bytes(path_meta, meta_bytes, meta_len);
    if (!ok) {
        /* A mid-write failure would otherwise leave a truncated .3sr or an
         * orphan sidecar with no partner. Unlink both, best-effort — a
         * half-written pair must never linger. remove() no-ops on a path that
         * was never created. */
        remove(path_3sr);
        remove(path_meta);
        snprintf(detail, detail_sz, "game_%d: write failed under %s", game_index, quark_dir);
    } else {
        game_out->game_index = game_index;
        rp_strlcpy(game_out->path_3sr, path_3sr, sizeof(game_out->path_3sr));
        rp_strlcpy(game_out->path_meta, path_meta, sizeof(game_out->path_meta));
        game_out->size_3sr = sr_len;
        game_out->size_meta = meta_len;
    }

    free(sr_bytes);
    free(meta_bytes);
    return ok;
}

/* Checks the {ok, error?, detail?} envelope common to every get3sr response.
 * Returns cJSON root on ok:true (caller cJSON_Delete's it), or NULL after
 * setting *err/detail. */
static cJSON* get3sr_check_envelope(const char* text, size_t len, RpError* err, char* detail, size_t detail_sz) {
    cJSON* root = cJSON_ParseWithLength(text, len);
    if (root == NULL) {
        *err = RP_ERR_PROTOCOL;
        rp_strlcpy(detail, "unparseable response JSON", detail_sz);
        return NULL;
    }
    const cJSON* ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok)) {
        *err = RP_ERR_PROTOCOL;
        rp_strlcpy(detail, "response missing boolean 'ok'", detail_sz);
        cJSON_Delete(root);
        return NULL;
    }
    if (!cJSON_IsTrue(ok)) {
        const cJSON* e = cJSON_GetObjectItemCaseSensitive(root, "error");
        *err = error_from_string(cJSON_IsString(e) ? e->valuestring : NULL);
        copy_str_field(root, "detail", detail, detail_sz);
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* Parses a MANIFEST response (metadata only) into an ascending list of the
 * game_indices the quark offers. Returns 0 on success (*n_out set), 1 on
 * failure (*err/detail set). */
static int parse_get3sr_manifest(const char* text, size_t len, int* indices, int* n_out, RpError* err, char* detail,
                                 size_t detail_sz) {
    *n_out = 0;
    cJSON* root = get3sr_check_envelope(text, len, err, detail, detail_sz);
    if (root == NULL)
        return 1;

    const cJSON* games = cJSON_GetObjectItemCaseSensitive(root, "games");
    if (!cJSON_IsArray(games) || cJSON_GetArraySize(games) == 0) {
        *err = RP_ERR_PROTOCOL;
        rp_strlcpy(detail, "manifest missing non-empty 'games' array", detail_sz);
        cJSON_Delete(root);
        return 1;
    }

    int n = 0;
    const cJSON* item = NULL;
    cJSON_ArrayForEach(item, games) {
        if (n >= RP_3SR_MAX_GAMES)
            break;
        if (!cJSON_IsObject(item))
            continue;
        const cJSON* gi = cJSON_GetObjectItemCaseSensitive(item, "game_index");
        if (!cJSON_IsNumber(gi))
            continue;
        indices[n++] = (int)gi->valuedouble;
    }
    cJSON_Delete(root);

    if (n == 0) {
        *err = RP_ERR_PROTOCOL;
        rp_strlcpy(detail, "manifest listed no usable game_index values", detail_sz);
        return 1;
    }
    *n_out = n;
    *err = RP_ERR_NONE;
    return 0;
}

/* Parses a PER-GAME response (exactly one game with b64+meta_b64) and writes
 * that one game's .3sr + .meta.json to disk. Returns 0 on success (*game_out
 * filled), 1 on failure (*err/detail set). */
static int parse_get3sr_single(const char* text, size_t len, const char* replay_root, const char* quarkid,
                               Rp3srGame* game_out, RpError* err, char* detail, size_t detail_sz) {
    cJSON* root = get3sr_check_envelope(text, len, err, detail, detail_sz);
    if (root == NULL)
        return 1;

    const cJSON* games = cJSON_GetObjectItemCaseSensitive(root, "games");
    if (!cJSON_IsArray(games) || cJSON_GetArraySize(games) == 0) {
        *err = RP_ERR_PROTOCOL;
        rp_strlcpy(detail, "ok response missing non-empty 'games' array", detail_sz);
        cJSON_Delete(root);
        return 1;
    }

    /* A per-game response carries exactly one game; take the first object. */
    const cJSON* item = cJSON_GetArrayItem(games, 0);
    if (!cJSON_IsObject(item)) {
        *err = RP_ERR_PROTOCOL;
        rp_strlcpy(detail, "games[0] is not an object", detail_sz);
        cJSON_Delete(root);
        return 1;
    }
    if (!fetch3sr_write_game(item, replay_root, quarkid, game_out, detail, detail_sz)) {
        *err = RP_ERR_PROTOCOL; /* detail already set by fetch3sr_write_game */
        cJSON_Delete(root);
        return 1;
    }

    cJSON_Delete(root);
    *err = RP_ERR_NONE;
    return 0;
}

/* Builds+sends one get3sr request (game_index < 0 => MANIFEST; >= 0 =>
 * PER-GAME) and returns the raw response body via proxy_wire_roundtrip. Returns
 * 0 on success (body/body_len set, caller frees the body), 1 on failure. */
static int get3sr_request(const Rp3srParams* params, int game_index, char** body, size_t* body_len, RpError* err,
                          char* detail, size_t detail_sz) {
    cJSON* req = cJSON_CreateObject();
    if (req == NULL) {
        *err = RP_ERR_PROTOCOL;
        rp_strlcpy(detail, "out of memory building request", detail_sz);
        return 1;
    }
    cJSON_AddStringToObject(req, "op", "get3sr");
    cJSON_AddStringToObject(req, "quarkid", params->quarkid);
    if (game_index >= 0)
        cJSON_AddNumberToObject(req, "game_index", game_index);

    char* json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (json == NULL) {
        *err = RP_ERR_PROTOCOL;
        rp_strlcpy(detail, "out of memory serializing request", detail_sz);
        return 1;
    }

    const size_t json_len = strlen(json);
    int rc = proxy_wire_roundtrip(params->host, params->port, json, json_len, body, body_len, err, detail, detail_sz);
    free(json);
    return rc;
}

/* Fetches exactly one game (game_index >= 0) in a single request/response and
 * writes it. */
static int fetch3sr_one(const Rp3srParams* params, int game_index, Rp3srGame* game_out, RpError* err, char* detail,
                        size_t detail_sz) {
    char* body = NULL;
    size_t body_len = 0;
    if (get3sr_request(params, game_index, &body, &body_len, err, detail, detail_sz) != 0)
        return 1;
    int rc = parse_get3sr_single(body, body_len, params->replay_root, params->quarkid, game_out, err, detail, detail_sz);
    free(body);
    return rc;
}

int RpFetch3sr(const Rp3srParams* params, Rp3srResults* out) {
    memset(out, 0, sizeof(*out));
    out->error = RP_ERR_PROTOCOL;

    if (params == NULL || params->host[0] == '\0') {
        out->error = RP_ERR_DISABLED;
        rp_strlcpy(out->detail, "no replay-proxy-host configured", sizeof(out->detail));
        return 1;
    }
    if (params->quarkid[0] == '\0' || params->replay_root[0] == '\0') {
        out->error = RP_ERR_BAD_REQUEST;
        rp_strlcpy(out->detail, "quarkid/replay_root required", sizeof(out->detail));
        return 1;
    }

    /* Single-game mode: one bounded request/response, one file pair written. */
    if (params->game_index >= 0) {
        if (fetch3sr_one(params, params->game_index, &out->games[0], &out->error, out->detail, sizeof(out->detail)) != 0)
            return 1;
        out->count = 1;
        out->error = RP_ERR_NONE;
        out->detail[0] = '\0';
        return 0;
    }

    /* Fetch-all mode: request the metadata-only MANIFEST first (one tiny frame
     * regardless of game count), then LOOP requesting each listed game
     * individually so every frame stays bounded (Stage S1 P-1). A game that
     * errors mid-loop is skipped + logged, not fatal — only zero successes
     * fails the whole set. */
    int indices[RP_3SR_MAX_GAMES];
    int n_indices = 0;
    {
        char* body = NULL;
        size_t body_len = 0;
        if (get3sr_request(params, -1, &body, &body_len, &out->error, out->detail, sizeof(out->detail)) != 0)
            return 1;
        int rc = parse_get3sr_manifest(body, body_len, indices, &n_indices, &out->error, out->detail,
                                       sizeof(out->detail));
        free(body);
        if (rc != 0)
            return 1;
    }

    int written = 0;
    RpError last_err = RP_ERR_NOT_FOUND;
    char last_detail[256];
    rp_strlcpy(last_detail, "no games could be fetched", sizeof(last_detail));

    for (int i = 0; i < n_indices && written < RP_3SR_MAX_GAMES; i++) {
        RpError gerr = RP_ERR_NONE;
        char gdetail[256];
        gdetail[0] = '\0';
        if (fetch3sr_one(params, indices[i], &out->games[written], &gerr, gdetail, sizeof(gdetail)) == 0) {
            written++;
        } else {
            last_err = gerr;
            rp_strlcpy(last_detail, gdetail, sizeof(last_detail));
            fprintf(stderr, "replay_proxy: get3sr %s game_%d skipped: %s\n", params->quarkid, indices[i], gdetail);
        }
    }

    if (written == 0) {
        out->error = last_err;
        rp_strlcpy(out->detail, last_detail, sizeof(out->detail));
        return 1;
    }

    out->count = written;
    out->error = RP_ERR_NONE;
    out->detail[0] = '\0';
    return 0;
}

/* ---- Async get3sr (single in-flight, polled state) — a SEPARATE slot from
 * the search worker, so a fetch and a search can run concurrently. ---------- */

static pthread_t s_3sr_thread;
static bool s_3sr_thread_valid = false;
static RpAtomic s_3sr_async_state = { PTHREAD_MUTEX_INITIALIZER, RP_ASYNC_IDLE };
static Rp3srParams s_3sr_async_params;
static Rp3srResults s_3sr_async_results;

static void* proxy_3sr_worker(void* unused) {
    (void)unused;
    RpFetch3sr(&s_3sr_async_params, &s_3sr_async_results);
    atomic_set(&s_3sr_async_state, RP_ASYNC_DONE);
    return NULL;
}

bool RpFetch3srAsync(const Rp3srParams* params) {
    if (params == NULL)
        return false;
    if (atomic_get(&s_3sr_async_state) != RP_ASYNC_IDLE)
        return false;

    if (s_3sr_thread_valid) {
        pthread_join(s_3sr_thread, NULL);
        s_3sr_thread_valid = false;
    }

    s_3sr_async_params = *params;
    memset(&s_3sr_async_results, 0, sizeof(s_3sr_async_results));
    atomic_set(&s_3sr_async_state, RP_ASYNC_RUNNING);

    if (pthread_create(&s_3sr_thread, NULL, proxy_3sr_worker, NULL) != 0) {
        atomic_set(&s_3sr_async_state, RP_ASYNC_IDLE);
        fprintf(stderr, "replay_proxy: failed to spawn get3sr thread\n");
        return false;
    }
    s_3sr_thread_valid = true;
    return true;
}

RpAsyncState RpFetch3srPoll(void) {
    return (RpAsyncState)atomic_get(&s_3sr_async_state);
}

bool RpFetch3srTake(Rp3srResults* out) {
    if (atomic_get(&s_3sr_async_state) != RP_ASYNC_DONE)
        return false;

    if (s_3sr_thread_valid) {
        pthread_join(s_3sr_thread, NULL);
        s_3sr_thread_valid = false;
    }
    if (out != NULL)
        *out = s_3sr_async_results;
    atomic_set(&s_3sr_async_state, RP_ASYNC_IDLE);
    return true;
}

void RpFetch3srCancel(void) {
    if (s_3sr_thread_valid) {
        pthread_join(s_3sr_thread, NULL);
        s_3sr_thread_valid = false;
    }
    atomic_set(&s_3sr_async_state, RP_ASYNC_IDLE);
}

/* ---- convertstatus (Stage S5) ------------------------------------------- */

/* Map the proxy's job `state` string to the RpConvertState enum. An unknown
 * ok:true state is UNKNOWN (rendered as a neutral line) rather than an error. */
static RpConvertState convert_state_from_string(const char* s) {
    if (s == NULL)
        return RP_CONVERT_UNKNOWN;
    if (strcmp(s, "absent") == 0)
        return RP_CONVERT_ABSENT;
    if (strcmp(s, "queued") == 0)
        return RP_CONVERT_QUEUED;
    if (strcmp(s, "pulling") == 0)
        return RP_CONVERT_PULLING;
    if (strcmp(s, "converting") == 0)
        return RP_CONVERT_CONVERTING;
    if (strcmp(s, "ready") == 0)
        return RP_CONVERT_READY;
    if (strcmp(s, "failed") == 0)
        return RP_CONVERT_FAILED;
    return RP_CONVERT_UNKNOWN;
}

/* Parse a convertstatus response body: {ok, state, progress, [detail]}. On
 * ok:false, out->error is set from the proxy's typed error; on ok:true,
 * out->state/progress are filled and out->error is RP_ERR_NONE. */
static void parse_convertstatus_response(const char* text, size_t len, RpConvertStatusResult* out) {
    cJSON* root = cJSON_ParseWithLength(text, len);
    if (root == NULL) {
        out->error = RP_ERR_PROTOCOL;
        rp_strlcpy(out->detail, "unparseable convertstatus response", sizeof(out->detail));
        return;
    }
    const cJSON* ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok)) {
        out->error = RP_ERR_PROTOCOL;
        rp_strlcpy(out->detail, "convertstatus response missing ok", sizeof(out->detail));
        cJSON_Delete(root);
        return;
    }
    if (!cJSON_IsTrue(ok)) {
        const cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
        out->error = error_from_string(cJSON_IsString(err) ? err->valuestring : NULL);
        copy_str_field(root, "detail", out->detail, sizeof(out->detail));
        cJSON_Delete(root);
        return;
    }

    char state_str[32];
    copy_str_field(root, "state", state_str, sizeof(state_str));
    out->state = convert_state_from_string(state_str[0] ? state_str : NULL);
    long long prog = number_field(root, "progress");
    if (prog < 0)
        prog = 0;
    if (prog > 100)
        prog = 100;
    out->progress = (int)prog;
    copy_str_field(root, "detail", out->detail, sizeof(out->detail));
    out->error = RP_ERR_NONE;
    cJSON_Delete(root);
}

int RpConvertStatus(const RpConvertStatusParams* params, RpConvertStatusResult* out) {
    memset(out, 0, sizeof(*out));
    out->error = RP_ERR_PROTOCOL;
    out->state = RP_CONVERT_UNKNOWN;

    if (params == NULL || params->host[0] == '\0') {
        out->error = RP_ERR_DISABLED;
        rp_strlcpy(out->detail, "no replay-proxy-host configured", sizeof(out->detail));
        return 1;
    }
    if (params->quarkid[0] == '\0') {
        out->error = RP_ERR_BAD_REQUEST;
        rp_strlcpy(out->detail, "quarkid required", sizeof(out->detail));
        return 1;
    }

    cJSON* req = cJSON_CreateObject();
    if (req == NULL) {
        out->error = RP_ERR_PROTOCOL;
        return 1;
    }
    cJSON_AddStringToObject(req, "op", "convertstatus");
    cJSON_AddStringToObject(req, "quarkid", params->quarkid);

    char* json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (json == NULL) {
        out->error = RP_ERR_PROTOCOL;
        return 1;
    }
    const size_t json_len = strlen(json);

    char* body = NULL;
    size_t body_len = 0;
    if (proxy_wire_roundtrip(params->host, params->port, json, json_len, &body, &body_len, &out->error, out->detail,
                             sizeof(out->detail)) != 0) {
        free(json);
        return 1;
    }
    free(json);

    parse_convertstatus_response(body, body_len, out);
    free(body);

    return out->error == RP_ERR_NONE ? 0 : 1;
}

/* ---- Async convertstatus — a THIRD in-flight slot, independent of search and
 * get3sr, so all three can run concurrently. ------------------------------- */

static pthread_t s_cs_thread;
static bool s_cs_thread_valid = false;
static RpAtomic s_cs_async_state = { PTHREAD_MUTEX_INITIALIZER, RP_ASYNC_IDLE };
static RpConvertStatusParams s_cs_async_params;
static RpConvertStatusResult s_cs_async_results;

static void* proxy_cs_worker(void* unused) {
    (void)unused;
    RpConvertStatus(&s_cs_async_params, &s_cs_async_results);
    atomic_set(&s_cs_async_state, RP_ASYNC_DONE);
    return NULL;
}

bool RpConvertStatusAsync(const RpConvertStatusParams* params) {
    if (params == NULL)
        return false;
    if (atomic_get(&s_cs_async_state) != RP_ASYNC_IDLE)
        return false;

    if (s_cs_thread_valid) {
        pthread_join(s_cs_thread, NULL);
        s_cs_thread_valid = false;
    }

    s_cs_async_params = *params;
    memset(&s_cs_async_results, 0, sizeof(s_cs_async_results));
    atomic_set(&s_cs_async_state, RP_ASYNC_RUNNING);

    if (pthread_create(&s_cs_thread, NULL, proxy_cs_worker, NULL) != 0) {
        atomic_set(&s_cs_async_state, RP_ASYNC_IDLE);
        fprintf(stderr, "replay_proxy: failed to spawn convertstatus thread\n");
        return false;
    }
    s_cs_thread_valid = true;
    return true;
}

RpAsyncState RpConvertStatusPoll(void) {
    return (RpAsyncState)atomic_get(&s_cs_async_state);
}

bool RpConvertStatusTake(RpConvertStatusResult* out) {
    if (atomic_get(&s_cs_async_state) != RP_ASYNC_DONE)
        return false;

    if (s_cs_thread_valid) {
        pthread_join(s_cs_thread, NULL);
        s_cs_thread_valid = false;
    }
    if (out != NULL)
        *out = s_cs_async_results;
    atomic_set(&s_cs_async_state, RP_ASYNC_IDLE);
    return true;
}

void RpConvertStatusCancel(void) {
    if (s_cs_thread_valid) {
        pthread_join(s_cs_thread, NULL);
        s_cs_thread_valid = false;
    }
    atomic_set(&s_cs_async_state, RP_ASYNC_IDLE);
}

const char* RpErrorHint(RpError err) {
    switch (err) {
    case RP_ERR_NONE:
        return "";
    case RP_ERR_DISABLED:
        return "REMOTE DISABLED";
    case RP_ERR_CONNECT:
        return "PROXY OFFLINE";
    case RP_ERR_PROTOCOL:
        return "PROXY ERROR";
    case RP_ERR_COOKIE_MISSING:
        return "CATALOG STALE";
    case RP_ERR_CLOUDFLARE_403:
        return "CATALOG STALE";
    case RP_ERR_UPSTREAM:
        return "TRY AGAIN LATER";
    case RP_ERR_BAD_REQUEST:
        return "PROXY ERROR";
    case RP_ERR_NOT_FOUND:
        return "NOT AVAILABLE";
    }
    return "PROXY ERROR";
}

/* ---------------------------------------------------------------------- */
/* Wrapper-side config accessor                                            */
/* ---------------------------------------------------------------------- */

/* Trim leading + trailing ASCII whitespace in place. */
static void rp_trim(char* s) {
    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

static int ascii_casecmp(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb)
            return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

bool RpConfigLoadFrom(const char* config_path, RpProxyConfig* out) {
    if (out == NULL)
        return false;
    out->host[0] = '\0';
    out->port = 3479; /* config.c default DEFAULT proxy port */

    if (config_path == NULL)
        return false;

    FILE* f = fopen(config_path, "r");
    if (f == NULL)
        return false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* cursor = line;
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (*cursor == '\0' || *cursor == '#' || *cursor == '\n' || *cursor == '\r')
            continue;

        char* equals = strchr(cursor, '=');
        if (equals == NULL)
            continue;
        *equals = '\0';
        char* key = cursor;
        char* val = equals + 1;
        rp_trim(key);
        rp_trim(val);
        if (key[0] == '\0')
            continue;

        if (ascii_casecmp(key, "replay-proxy-host") == 0) {
            rp_strlcpy(out->host, val, sizeof(out->host));
        } else if (ascii_casecmp(key, "replay-proxy-port") == 0) {
            int p = atoi(val);
            if (p > 0 && p <= 65535)
                out->port = p;
        }
    }

    fclose(f);
    return true;
}

bool RpConfigLoad(RpProxyConfig* out) {
    return RpConfigLoadFrom(RP_CONFIG_PATH, out);
}
