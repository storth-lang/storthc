#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "utils/st_arena.h"
#include "utils/st_helper.h"
#include "utils/st_ht.h"
#include "utils/st_process.h"
#include "utils/st_string.h"

#define RECORD_DB_PATH "record.sth"
#define DB_MAGIC_V1 "STHREC01"
#define DB_MAGIC_V2 "STHREC02"
#define SHA_HEX 64
#define SHA_RAW 32
#define MAX_DIFF_LINES 12

#define C_GREEN "\x1b[32m"
#define C_BOLD_GREEN "\x1b[1m\x1b[32m"
#define C_BOLD_CYAN "\x1b[1m\x1b[36m"
#define C_BOLD_YELLOW "\x1b[1m\x1b[33m"

static const u8 pad4[] = {0x00, 0xFF, 0x00, 0xFF};
static const u8 pad8[] = {0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF};

static b8 g_color;

static const char *col(const char *code) {
    return g_color ? code : "";
}

typedef enum {
    ENTRY_SET,
    ENTRY_RECORDED,
} entry_state_t;

typedef struct {
    entry_state_t kind;
    char *file_name;
    char *cmd;
    u8 sha_of_file[SHA_RAW];
    i32 expected_status;
    char *result;
    u32 result_len;
} test_entry_t;

typedef struct {
    ST_arena_t *arena;
    test_entry_t *items;
    u32 count;
    u32 capacity;
    ST_ht_t by_name;
} test_db_t;

static char *arena_strdup(ST_arena_t *a, const char *s) {
    u32 n = (u32)strlen(s);
    char *p = ST_arena_push(a, n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static const char *canon_rel(const char *p) {
    while (p[0] == '.' && p[1] == '/') p += 2;
    return p;
}

static ST_ht_generic_t *name_key(ST_arena_t *a, const char *name) {
    ST_ht_generic_t *k = ST_arena_push(a, sizeof(*k));
    k->tag = (void *)name;
    k->size = (u32)strlen(name);
    return k;
}

static test_entry_t *db_find(test_db_t *db, const char *name) {
    ST_ht_generic_t key = {.tag = (void *)canon_rel(name), .size = (u32)strlen(canon_rel(name))};
    u32 *idx = (u32 *)ST_ht_get(&db->by_name, key).tag;
    return idx ? &db->items[*idx] : NULL;
}

static test_entry_t *db_push(test_db_t *db, const char *name) {
    test_entry_t blank;
    memset(&blank, 0, sizeof(blank));
    blank.file_name = arena_strdup(db->arena, canon_rel(name));
    blank.cmd = "";
    blank.result = "";
    ST_da_append_arena(db->arena, db, blank);

    u32 *idx = ST_arena_push(db->arena, sizeof(*idx));
    *idx = db->count - 1;
    ST_ht_set(&db->by_name, name_key(db->arena, db->items[*idx].file_name),
              (ST_ht_generic_t){.tag = idx, .size = sizeof(*idx)});
    return &db->items[*idx];
}

static u32 codec_state;

static void codec_reset(void) {
    codec_state = 0x53544831;
}

static u8 codec_next(void) {
    codec_state ^= codec_state << 13;
    codec_state ^= codec_state >> 17;
    codec_state ^= codec_state << 5;
    return (u8)codec_state;
}

static b8 v1_read_bytes(FILE *f, void *p, size_t n) {
    if (fread(p, 1, n, f) != n) return 0;
    u8 *b = p;
    for (size_t i = 0; i < n; i++) b[i] ^= codec_next();
    return 1;
}

static b8 v1_read_u32(FILE *f, u32 *v) {
    u8 b[4];
    if (!v1_read_bytes(f, b, sizeof(b))) return 0;
    *v = (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
    return 1;
}

static char *v1_read_str(FILE *f, ST_arena_t *a, u32 *out_len) {
    u32 len;
    if (!v1_read_u32(f, &len)) return NULL;
    char *s = ST_arena_push(a, len + 1);
    if (!v1_read_bytes(f, s, len)) return NULL;
    s[len] = 0;
    if (out_len) *out_len = len;
    return s;
}

static b8 v1_read_pad(FILE *f, const u8 *pad, size_t n) {
    u8 b[8];
    return v1_read_bytes(f, b, n) && memcmp(b, pad, n) == 0;
}

static b8 hex_to_raw(const char *hex, u8 out[SHA_RAW]) {
    for (u32 i = 0; i < SHA_RAW; i++) {
        u32 v = 0;
        for (u32 j = 0; j < 2; j++) {
            char c = hex[i * 2 + j];
            u32 d;
            if (c >= '0' && c <= '9') d = (u32)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
            else return 0;
            v = v << 4 | d;
        }
        out[i] = (u8)v;
    }
    return 1;
}

static u64 hash64(const u8 *p, size_t n) {
    const u64 mul = 0x9E3779B97F4A7C15ULL;
    u64 h = 0x243F6A8885A308D3ULL ^ ((u64)n * mul);
    while (n >= 8) {
        u64 w;
        memcpy(&w, p, 8);
        h = (h ^ w) * mul;
        h ^= h >> 29;
        p += 8;
        n -= 8;
    }
    u64 tail = 0;
    memcpy(&tail, p, n);
    h = (h ^ tail) * mul;
    h ^= h >> 32;
    return h;
}

static void db_init(test_db_t *db) {
    memset(db, 0, sizeof(*db));
    db->arena = ST_arena_alloc();
    ST_ht_init(db->arena, &db->by_name, 64);
}

static b8 db_load_v1(const u8 *data, size_t len, test_db_t *db, const char *db_path) {
    FILE *f = fmemopen((void *)data, len, "rb");
    if (!f) return 0;

    char magic[8];
    u32 count;
    codec_reset();
    if (fread(magic, 1, 8, f) != 8 || !v1_read_u32(f, &count)) {
        fprintf(stderr, "error: %s is corrupt or not a record db\n", db_path);
        fclose(f);
        return 0;
    }

    for (u32 i = 0; i < count; i++) {
        u8 kind;
        char *name = NULL;
        char *cmd = NULL;
        char sha[SHA_HEX + 1];
        i32 status = 0;
        char *result = NULL;
        u32 result_len = 0;
        b8 ok = v1_read_bytes(f, &kind, 1)
             && (name = v1_read_str(f, db->arena, NULL)) != NULL
             && v1_read_pad(f, pad8, 8)
             && (cmd = v1_read_str(f, db->arena, NULL)) != NULL
             && v1_read_pad(f, pad4, 4)
             && v1_read_bytes(f, sha, SHA_HEX + 1)
             && v1_read_pad(f, pad4, 4)
             && v1_read_u32(f, (u32 *)&status)
             && v1_read_pad(f, pad4, 4)
             && (result = v1_read_str(f, db->arena, &result_len)) != NULL
             && v1_read_pad(f, pad8, 8);
        if (!ok) {
            fprintf(stderr, "error: %s truncated at entry %u\n", db_path, i);
            fclose(f);
            return 0;
        }
        test_entry_t *e = db_push(db, name);
        e->kind = (entry_state_t)kind;
        e->cmd = cmd;
        if (!hex_to_raw(sha, e->sha_of_file)) memset(e->sha_of_file, 0, SHA_RAW);
        e->expected_status = status;
        e->result = result;
        e->result_len = result_len;
    }
    fclose(f);
    return 1;
}

typedef struct {
    const u8 *p;
    const u8 *end;
    b8 bad;
} cursor_t;

static u32 cur_u32(cursor_t *c) {
    if (c->end - c->p < 4) {
        c->bad = 1;
        return 0;
    }
    u32 v = (u32)c->p[0] | (u32)c->p[1] << 8 | (u32)c->p[2] << 16 | (u32)c->p[3] << 24;
    c->p += 4;
    return v;
}

static const u8 *cur_bytes(cursor_t *c, size_t n) {
    if ((size_t)(c->end - c->p) < n) {
        c->bad = 1;
        return NULL;
    }
    const u8 *r = c->p;
    c->p += n;
    return r;
}

static char *cur_str(cursor_t *c, u32 *out_len) {
    u32 len = cur_u32(c);
    if (c->bad) return NULL;
    const u8 *s = cur_bytes(c, (size_t)len + 1);
    if (!s || s[len] != 0) {
        c->bad = 1;
        return NULL;
    }
    if (out_len) *out_len = len;
    return (char *)s;
}

static b8 db_load_v2(const u8 *data, size_t len, test_db_t *db, const char *db_path) {
    if (len < 8 + 4 + 4 + 8) {
        fprintf(stderr, "error: %s is truncated\n", db_path);
        return 0;
    }
    u64 want;
    memcpy(&want, data + len - 8, 8);
    if (hash64(data, len - 8) != want) {
        fprintf(stderr, "error: %s failed its checksum (corrupt or edited by hand)\n", db_path);
        return 0;
    }

    cursor_t c = {.p = data + 8, .end = data + len - 8, .bad = 0};
    u32 count = cur_u32(&c);
    cur_u32(&c);

    for (u32 i = 0; i < count; i++) {
        const u8 *kind = cur_bytes(&c, 1);
        u32 status = cur_u32(&c);
        const u8 *sha = cur_bytes(&c, SHA_RAW);
        char *name = cur_str(&c, NULL);
        char *cmd = cur_str(&c, NULL);
        u32 result_len = 0;
        char *result = cur_str(&c, &result_len);
        if (c.bad || *kind > ENTRY_RECORDED) {
            fprintf(stderr, "error: %s malformed at entry %u\n", db_path, i);
            return 0;
        }
        test_entry_t *e = db_push(db, name);
        e->kind = (entry_state_t)*kind;
        e->cmd = cmd;
        memcpy(e->sha_of_file, sha, SHA_RAW);
        e->expected_status = (i32)status;
        e->result = result;
        e->result_len = result_len;
    }
    return 1;
}

static b8 db_load(const char *db_path, test_db_t *db, b8 *migrated) {
    db_init(db);
    *migrated = 0;

    FILE *probe = fopen(db_path, "rb");
    if (!probe) return 1;
    fclose(probe);

    ST_string_t file;
    if (!ST_read_entire_file(db->arena, &file, db_path)) {
        fprintf(stderr, "error: could not read %s\n", db_path);
        return 0;
    }
    if (file.len >= 8 && memcmp(file.data, DB_MAGIC_V2, 8) == 0) return db_load_v2(file.data, file.len, db, db_path);
    if (file.len >= 8 && memcmp(file.data, DB_MAGIC_V1, 8) == 0) {
        *migrated = 1;
        return db_load_v1(file.data, file.len, db, db_path);
    }
    fprintf(stderr, "error: %s is corrupt or not a record db\n", db_path);
    return 0;
}

static void put_u32(u8 **w, u32 v) {
    (*w)[0] = (u8)v;
    (*w)[1] = (u8)(v >> 8);
    (*w)[2] = (u8)(v >> 16);
    (*w)[3] = (u8)(v >> 24);
    *w += 4;
}

static void put_str(u8 **w, const char *s, u32 len) {
    put_u32(w, len);
    memcpy(*w, s, len);
    *w += len;
    **w = 0;
    *w += 1;
}

static b8 db_save(const char *path, test_db_t *db) {
    size_t size = 8 + 4 + 4 + 8;
    for (u32 i = 0; i < db->count; i++) {
        test_entry_t *e = &db->items[i];
        size += 1 + 4 + SHA_RAW + 3 * 5 + strlen(e->file_name) + strlen(e->cmd) + e->result_len;
    }

    u8 *buf = malloc(size);
    u8 *w = buf;
    memcpy(w, DB_MAGIC_V2, 8);
    w += 8;
    put_u32(&w, db->count);
    put_u32(&w, 0);
    for (u32 i = 0; i < db->count; i++) {
        test_entry_t *e = &db->items[i];
        *w++ = (u8)e->kind;
        put_u32(&w, (u32)e->expected_status);
        memcpy(w, e->sha_of_file, SHA_RAW);
        w += SHA_RAW;
        put_str(&w, e->file_name, (u32)strlen(e->file_name));
        put_str(&w, e->cmd, (u32)strlen(e->cmd));
        put_str(&w, e->result, e->result_len);
    }
    u64 sum = hash64(buf, (size_t)(w - buf));
    memcpy(w, &sum, 8);
    w += 8;

    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "error: could not open %s for writing\n", tmp);
        free(buf);
        return 0;
    }
    b8 ok = fwrite(buf, 1, (size_t)(w - buf), f) == (size_t)(w - buf);
    if (fclose(f) != 0) ok = 0;
    if (ok && rename(tmp, path) != 0) ok = 0;
    if (!ok) {
        remove(tmp);
        fprintf(stderr, "error: failed writing %s\n", path);
    }
    free(buf);
    return ok;
}

typedef struct {
    u32 h[8];
    u8 buf[64];
    u64 total;
    u32 fill;
} sha256_t;

static const u32 sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static u32 rotr32(u32 x, u32 n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(sha256_t *s, const u8 *p) {
    u32 w[64];
    for (u32 i = 0; i < 16; i++)
        w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | (u32)p[i * 4 + 3];
    for (u32 i = 16; i < 64; i++) {
        u32 s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    u32 e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (u32 i = 0; i < 64; i++) {
        u32 s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + s1 + ch + sha_k[i] + w[i];
        u32 s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void sha256_init(sha256_t *s) {
    static const u32 init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, init, sizeof(init));
    s->total = 0;
    s->fill = 0;
}

static void sha256_update(sha256_t *s, const u8 *p, size_t n) {
    s->total += n;
    while (n > 0) {
        size_t take = 64 - s->fill;
        if (take > n) take = n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += (u32)take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha256_raw(sha256_t *s, u8 out[SHA_RAW]) {
    u64 bits = s->total * 8;
    u8 one = 0x80;
    u8 zero = 0;
    sha256_update(s, &one, 1);
    while (s->fill != 56) sha256_update(s, &zero, 1);
    u8 len[8];
    for (u32 i = 0; i < 8; i++) len[i] = (u8)(bits >> (56 - i * 8));
    sha256_update(s, len, 8);
    for (u32 i = 0; i < 8; i++) {
        out[i * 4] = (u8)(s->h[i] >> 24);
        out[i * 4 + 1] = (u8)(s->h[i] >> 16);
        out[i * 4 + 2] = (u8)(s->h[i] >> 8);
        out[i * 4 + 3] = (u8)s->h[i];
    }
}

static b8 sha256_of(const char *path, u8 out[SHA_RAW]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    sha256_t s;
    sha256_init(&s);
    u8 buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) sha256_update(&s, buf, got);
    b8 ok = !ferror(f);
    fclose(f);
    if (ok) sha256_raw(&s, out);
    return ok;
}

static f64 now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec * 1000.0 + (f64)ts.tv_nsec / 1e6;
}

static char *cmd_expand(const char *cmd, const char *file_name) {
    size_t flen = strlen(file_name);
    size_t cap = strlen(cmd) + 1;
    for (const char *p = cmd; (p = strstr(p, "$file")); p += 5) cap += flen;

    char *out = malloc(cap);
    char *w = out;
    const char *r = cmd;
    while (*r) {
        if (r[0] == '$' && strncmp(r, "$file", 5) == 0) {
            memcpy(w, file_name, flen);
            w += flen;
            r += 5;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
    return out;
}

static void exe_path_of(const char *file_name, char *out, size_t n) {
    if (strchr(file_name, '/')) snprintf(out, n, "%s.exe", file_name);
    else snprintf(out, n, "./%s.exe", file_name);
}

static i32 run_capture(const char *const *args, char **out, u32 *out_len) {
    FILE *tmp = tmpfile();
    if (!tmp) {
        *out = malloc(1);
        (*out)[0] = 0;
        *out_len = 0;
        return -1;
    }

    ST_proc_t proc;
    memset(&proc, 0, sizeof(proc));
    proc.id = (void *)-1;
    proc.opt.out = tmp;
    proc.opt.merge_err = 1;
    u32 n = 0;
    while (args[n] && n < MAX_ARGS) {
        proc.opt.args[n] = args[n];
        n += 1;
    }
    proc.opt.args[n] = NULL;

    ST_run_process(&proc);
    i32 status = proc.exited_normally ? proc.exit_code : -1;

    fflush(tmp);
    long size = ftell(tmp);
    if (size < 0) size = 0;
    rewind(tmp);
    char *buf = malloc((size_t)size + 1);
    size_t got = size > 0 ? fread(buf, 1, (size_t)size, tmp) : 0;
    buf[got] = 0;
    fclose(tmp);

    *out = buf;
    *out_len = (u32)got;
    return status;
}

typedef enum {
    JOB_OK,
    JOB_HASH_READ,
    JOB_HASH_MISMATCH,
    JOB_BUILD_FAILED,
} job_err_t;

typedef struct {
    const test_entry_t *entry;
    b8 done;
    job_err_t err;
    b8 have_exe;
    i32 status;
    char *out;
    u32 out_len;
    u8 sha[SHA_RAW];
    char *cmd_used;
    f64 ms;
    b8 passed;
} job_t;

typedef struct {
    b8 record_mode;
    job_t *jobs;
    u32 count;
    u32 next;
    u32 flushed;
    pthread_mutex_t mu;
} pool_t;

static void run_job(job_t *j, b8 record_mode) {
    const test_entry_t *e = j->entry;
    f64 t0 = now_ms();
    j->out = NULL;
    j->out_len = 0;

    if (!sha256_of(e->file_name, j->sha)) {
        j->err = JOB_HASH_READ;
        goto finish;
    }
    if (!record_mode && memcmp(j->sha, e->sha_of_file, SHA_RAW) != 0) {
        j->err = JOB_HASH_MISMATCH;
        goto finish;
    }

    char exe[PATH_MAX + 8];
    exe_path_of(e->file_name, exe, sizeof(exe));
    remove(exe);

    j->cmd_used = cmd_expand(e->cmd, e->file_name);
    const char *sh_args[] = {"sh", "-c", j->cmd_used, NULL};
    i32 status = run_capture(sh_args, &j->out, &j->out_len);

    j->have_exe = access(exe, F_OK) == 0;
    if (j->have_exe && status != 0) {
        j->err = JOB_BUILD_FAILED;
        j->status = status;
        remove(exe);
        goto finish;
    }
    if (j->have_exe) {
        free(j->out);
        const char *exe_args[] = {exe, NULL};
        status = run_capture(exe_args, &j->out, &j->out_len);
        remove(exe);
    }
    j->status = status;
    j->err = JOB_OK;

finish:
    j->ms = now_ms() - t0;
    if (!record_mode) {
        j->passed = j->err == JOB_OK && j->status == e->expected_status && j->out_len == e->result_len &&
                    (j->out_len == 0 || memcmp(j->out, e->result, j->out_len) == 0);
    } else {
        j->passed = j->err == JOB_OK;
    }
}

static void print_ms(f64 ms) {
    if (ms >= 1000.0) printf("  %s%.2fs%s", col(ST_COLOR_GREY), ms / 1000.0, col(ST_COLOR_RESET));
    else printf("  %s%.0fms%s", col(ST_COLOR_GREY), ms, col(ST_COLOR_RESET));
}

static void print_indented(const char *text, u32 len) {
    u32 start = 0;
    while (start < len) {
        u32 end = start;
        while (end < len && text[end] != '\n') end += 1;
        printf("       %s|%s %.*s\n", col(ST_COLOR_GREY), col(ST_COLOR_RESET), (int)(end - start), text + start);
        start = end + 1;
    }
}

static void print_diff(const char *expected, const char *got) {
    u32 line = 1;
    u32 shown = 0;
    u32 hidden = 0;
    while (*expected || *got) {
        size_t elen = strcspn(expected, "\n");
        size_t glen = strcspn(got, "\n");
        if (elen != glen || memcmp(expected, got, elen) != 0) {
            if (shown < MAX_DIFF_LINES) {
                printf("       %sline %u%s\n", col(ST_COLOR_GREY), line, col(ST_COLOR_RESET));
                printf("       %s- %.*s%s\n", col(ST_COLOR_RED), (int)elen, expected, col(ST_COLOR_RESET));
                printf("       %s+ %.*s%s\n", col(C_GREEN), (int)glen, got, col(ST_COLOR_RESET));
                shown += 1;
            } else {
                hidden += 1;
            }
        }
        expected += elen + (expected[elen] == '\n');
        got += glen + (got[glen] == '\n');
        line += 1;
    }
    if (hidden)
        printf("       %s... %u more differing lines%s\n", col(ST_COLOR_GREY), hidden, col(ST_COLOR_RESET));
}

static void print_job(const job_t *j, b8 record_mode) {
    const test_entry_t *e = j->entry;
    const char *name_open = col(ST_COLOR_BOLD);
    const char *reset = col(ST_COLOR_RESET);

    if (record_mode) {
        if (j->err == JOB_OK) {
            printf("[%sREC%s ] %s%s%s -> status %d, %u bytes of output%s", col(C_BOLD_CYAN), reset, name_open,
                   e->file_name, reset, j->status, j->out_len, j->have_exe ? "" : " (command output)");
            print_ms(j->ms);
            printf("\n");
            return;
        }
    } else if (j->passed) {
        printf("[%s OK %s] %s", col(C_BOLD_GREEN), reset, e->file_name);
        print_ms(j->ms);
        printf("\n");
        return;
    }

    printf("[%sFAIL%s] %s%s%s", col(ST_COLOR_BOLD_RED), reset, name_open, e->file_name, reset);
    print_ms(j->ms);
    printf("\n");

    if (j->err == JOB_HASH_READ) {
        printf("       could not read or hash the file\n");
    } else if (j->err == JOB_HASH_MISMATCH) {
        printf("       %sfile hash mismatch%s (file modified or missing), rerecord it\n", col(ST_COLOR_YELLOW), reset);
    } else if (j->err == JOB_BUILD_FAILED) {
        printf("       %sbuild failed%s (status %d): `%s`\n", col(ST_COLOR_YELLOW), reset, j->status, j->cmd_used);
        print_indented(j->out, j->out_len);
    } else {
        if (j->status != e->expected_status)
            printf("       %sstatus%s: expected %d, got %s%d%s\n", col(ST_COLOR_YELLOW), reset, e->expected_status,
                   col(ST_COLOR_RED), j->status, reset);
        if (!(j->out_len == e->result_len && (j->out_len == 0 || memcmp(j->out, e->result, j->out_len) == 0))) {
            printf("       %soutput mismatch%s (expected %u bytes, got %u bytes):\n", col(ST_COLOR_YELLOW), reset,
                   e->result_len, j->out_len);
            print_diff(e->result, j->out ? j->out : "");
        }
    }
}

static void flush_ready(pool_t *p) {
    while (p->flushed < p->count && p->jobs[p->flushed].done) {
        print_job(&p->jobs[p->flushed], p->record_mode);
        p->flushed += 1;
    }
    fflush(stdout);
}

static void *worker_main(void *arg) {
    pool_t *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        u32 i = p->next;
        if (i < p->count) p->next += 1;
        pthread_mutex_unlock(&p->mu);
        if (i >= p->count) break;

        run_job(&p->jobs[i], p->record_mode);

        pthread_mutex_lock(&p->mu);
        p->jobs[i].done = 1;
        flush_ready(p);
        pthread_mutex_unlock(&p->mu);
    }
    return NULL;
}

static u32 run_pool(job_t *jobs, u32 count, u32 threads, b8 record_mode) {
    pool_t pool;
    memset(&pool, 0, sizeof(pool));
    pool.record_mode = record_mode;
    pool.jobs = jobs;
    pool.count = count;
    pthread_mutex_init(&pool.mu, NULL);

    if (threads < 1) threads = 1;
    if (threads > count) threads = count;

    pthread_t *ids = malloc(sizeof(*ids) * threads);
    u32 started = 0;
    for (u32 i = 0; i < threads; i++) {
        if (pthread_create(&ids[i], NULL, worker_main, &pool) == 0) started += 1;
    }
    if (started == 0) worker_main(&pool);
    for (u32 i = 0; i < started; i++) pthread_join(ids[i], NULL);
    free(ids);
    pthread_mutex_destroy(&pool.mu);
    return started ? started : 1;
}

static void jobs_free(job_t *jobs, u32 count) {
    for (u32 i = 0; i < count; i++) {
        free(jobs[i].out);
        free(jobs[i].cmd_used);
    }
    free(jobs);
}

static job_t *jobs_for(test_db_t *db, b8 only_set, b8 only_recorded, u32 *count) {
    u32 n = 0;
    job_t *jobs = calloc(db->count ? db->count : 1, sizeof(*jobs));
    for (u32 i = 0; i < db->count; i++) {
        test_entry_t *e = &db->items[i];
        if (only_set && e->kind != ENTRY_SET) continue;
        if (only_recorded && e->kind != ENTRY_RECORDED) continue;
        jobs[n].entry = e;
        n += 1;
    }
    *count = n;
    return jobs;
}

static void print_summary(const char *what, u32 ran, u32 failed, f64 ms, u32 threads) {
    const char *state = failed == 0 ? col(C_BOLD_GREEN) : col(ST_COLOR_BOLD_RED);
    printf("%s%u %s, %u failed%s", state, ran, what, failed, col(ST_COLOR_RESET));
    printf("  %s(%.2fs, %u threads)%s\n", col(ST_COLOR_GREY), ms / 1000.0, threads, col(ST_COLOR_RESET));
}

static b8 record_jobs(test_db_t *db, const char *path, job_t *jobs, u32 n, u32 threads) {
    if (n == 0) {
        printf("record: nothing to do (no set-but-unrecorded files)\n");
        return 1;
    }
    f64 t0 = now_ms();
    u32 used = run_pool(jobs, n, threads, 1);

    u32 failed = 0;
    for (u32 i = 0; i < n; i++) {
        job_t *j = &jobs[i];
        if (!j->passed) {
            failed += 1;
            continue;
        }
        test_entry_t *e = (test_entry_t *)j->entry;
        memcpy(e->sha_of_file, j->sha, SHA_RAW);
        e->expected_status = j->status;
        char *copy = ST_arena_push(db->arena, j->out_len + 1);
        memcpy(copy, j->out, j->out_len);
        copy[j->out_len] = 0;
        e->result = copy;
        e->result_len = j->out_len;
        e->kind = ENTRY_RECORDED;
    }
    print_summary("recorded", n, failed, now_ms() - t0, used);
    b8 saved = db_save(path, db);
    return saved && failed == 0;
}

static b8 cmd_record(const char *path, test_db_t *db, u32 threads) {
    u32 n;
    job_t *jobs = jobs_for(db, 1, 0, &n);
    b8 ok = record_jobs(db, path, jobs, n, threads);
    jobs_free(jobs, n);
    return ok;
}

static b8 cmd_rerecord(test_db_t *db, const char *path, const char *file_name, const char *new_cmd, u32 threads) {
    u32 n = 0;
    job_t *jobs;
    if (file_name) {
        test_entry_t *e = db_find(db, file_name);
        if (!e) {
            fprintf(stderr, "error: no test for %s, use -set first\n", file_name);
            return 0;
        }
        if (new_cmd) e->cmd = arena_strdup(db->arena, new_cmd);
        jobs = calloc(1, sizeof(*jobs));
        jobs[0].entry = e;
        n = 1;
    } else {
        jobs = jobs_for(db, 0, 0, &n);
    }
    b8 ok = record_jobs(db, path, jobs, n, threads);
    jobs_free(jobs, n);
    return ok;
}

static b8 cmd_verify(test_db_t *db, u32 threads) {
    u32 n;
    job_t *jobs = jobs_for(db, 0, 1, &n);

    u32 unrecorded = 0;
    for (u32 i = 0; i < db->count; i++)
        if (db->items[i].kind != ENTRY_RECORDED) unrecorded += 1;

    f64 t0 = now_ms();
    u32 used = n ? run_pool(jobs, n, threads, 0) : 1;

    u32 failed = 0;
    for (u32 i = 0; i < n; i++)
        if (!jobs[i].passed) failed += 1;

    print_summary("ran", n, failed, now_ms() - t0, used);
    if (unrecorded)
        printf("%s%u set but not recorded, run -record%s\n", col(C_BOLD_YELLOW), unrecorded, col(ST_COLOR_RESET));
    jobs_free(jobs, n);
    return failed == 0;
}

static const char *resolve_test_file(ST_arena_t *a, const char *start_cwd, const char *db_dir, const char *arg) {
    char joined[PATH_MAX * 2];
    if (arg[0] == '/') snprintf(joined, sizeof(joined), "%s", arg);
    else snprintf(joined, sizeof(joined), "%s/%s", start_cwd, arg);

    char real[PATH_MAX];
    if (!realpath(joined, real)) return NULL;

    size_t dl = strlen(db_dir);
    if (strncmp(real, db_dir, dl) == 0 && real[dl] == '/') return arena_strdup(a, real + dl + 1);
    return arena_strdup(a, real);
}

static b8 cmd_set(test_db_t *db, const char *path, const char **file_names, u32 n, const char *cmd) {
    for (u32 i = 0; i < n; i++) {
        test_entry_t *e = db_find(db, file_names[i]);
        if (!e) {
            e = db_push(db, file_names[i]);
        } else {
            e->kind = ENTRY_SET;
        }
        e->cmd = arena_strdup(db->arena, cmd);
        printf("%sset%s: %s -> `%s`\n", col(C_BOLD_CYAN), col(ST_COLOR_RESET), e->file_name, cmd);
    }
    return db_save(path, db);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage:\n"
            "  %s [-dir <d>] [-file <db>] [-j <n>] -set -f <file>... -cmd \"<cmd with $file>\"\n"
            "  %s [-dir <d>] [-file <db>] [-j <n>] -record\n"
            "  %s [-dir <d>] [-file <db>] [-j <n>] -rerecord -f <file> [cmd]\n"
            "  %s [-dir <d>] [-file <db>] [-j <n>] -rerecord -all\n"
            "  %s [-dir <d>] [-file <db>] [-j <n>]\n"
            "  %s -help\n",
            prog, prog, prog, prog, prog, prog);
}

static void help(const char *prog) {
    printf(
        "%s - parallel snapshot test framework\n"
        "\n"
        "  %s\n"
        "      Verify all recorded tests in parallel: check the file hash, build, run\n"
        "      the exe, and report any mismatch in exit status or output.\n"
        "\n"
        "  %s -set -f <file>... -cmd '<build cmd>'\n"
        "      Register one or more files with a build command (one db write in total). $file in the cmd expands to the\n"
        "      file name relative to the record db directory (use single quotes so the\n"
        "      shell does not expand it). If the cmd produces <file>.exe that exe is run\n"
        "      and its output is the snapshot, otherwise the cmd output is.\n"
        "\n"
        "  %s -record\n"
        "      Build and run every set-but-unrecorded file in parallel, snapshot its\n"
        "      exit status and stdout+stderr, and store the file hash.\n"
        "\n"
        "  %s -rerecord -f <file> [cmd]\n"
        "      Rerecord one test. If cmd is given it replaces the set cmd.\n"
        "\n"
        "  %s -rerecord -all\n"
        "      Rerecord every test, keeping file names and cmds.\n"
        "\n"
        "options (before the command):\n"
        "  -dir <d>      chdir to <d> first\n"
        "  -file <db>    record db to use (default %s). The tester always runs from the\n"
        "                db's directory, so it can be started from anywhere and relative\n"
        "                cmds like ../build/storthc keep working.\n"
        "  -j <n>        number of worker threads (default: number of cpus)\n"
        "  -nocolor      disable colors (also off when not a tty or NO_COLOR is set)\n"
        "  -color        force colors\n"
        "\n"
        "File arguments are resolved to absolute paths and stored relative to the db\n"
        "directory when they live under it, so the db stays portable between checkouts.\n",
        prog, prog, prog, prog, prog, prog, RECORD_DB_PATH);
}

int main(int argc, char *argv[]) {
    const char *db_arg = RECORD_DB_PATH;
    u32 threads = 0;
    i32 color_mode = -1;
    int i = 1;

    for (;;) {
        if (i + 1 < argc && strcmp(argv[i], "-dir") == 0) {
            if (chdir(argv[i + 1]) != 0) {
                fprintf(stderr, "error: cannot chdir to %s\n", argv[i + 1]);
                return 1;
            }
            i += 2;
        } else if (i + 1 < argc && strcmp(argv[i], "-file") == 0) {
            db_arg = argv[i + 1];
            i += 2;
        } else if (i + 1 < argc && strcmp(argv[i], "-j") == 0) {
            threads = (u32)atoi(argv[i + 1]);
            i += 2;
        } else if (i < argc && strncmp(argv[i], "-j", 2) == 0 && isdigit((unsigned char)argv[i][2])) {
            threads = (u32)atoi(argv[i] + 2);
            i += 1;
        } else if (i < argc && strcmp(argv[i], "-nocolor") == 0) {
            color_mode = 0;
            i += 1;
        } else if (i < argc && strcmp(argv[i], "-color") == 0) {
            color_mode = 1;
            i += 1;
        } else {
            break;
        }
    }

    g_color = color_mode >= 0 ? (b8)color_mode : (isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL);

    if (threads == 0) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        threads = cpus > 0 ? (u32)cpus : 1;
    }

    char start_cwd[PATH_MAX];
    if (!getcwd(start_cwd, sizeof(start_cwd))) {
        fprintf(stderr, "error: cannot determine the current directory\n");
        return 1;
    }

    char db_abs[PATH_MAX * 2];
    if (db_arg[0] == '/') snprintf(db_abs, sizeof(db_abs), "%s", db_arg);
    else snprintf(db_abs, sizeof(db_abs), "%s/%s", start_cwd, db_arg);

    char *slash = strrchr(db_abs, '/');
    char db_dir[PATH_MAX];
    char db_base[PATH_MAX];
    if (slash == db_abs) {
        snprintf(db_dir, sizeof(db_dir), "/");
    } else {
        char tmp[PATH_MAX * 2];
        snprintf(tmp, sizeof(tmp), "%.*s", (int)(slash - db_abs), db_abs);
        if (!realpath(tmp, db_dir)) {
            fprintf(stderr, "error: record db directory %s does not exist\n", tmp);
            return 1;
        }
    }
    snprintf(db_base, sizeof(db_base), "%s", slash + 1);

    if (chdir(db_dir) != 0) {
        fprintf(stderr, "error: cannot chdir to %s\n", db_dir);
        return 1;
    }

    test_db_t db;
    b8 migrated = 0;
    if (!db_load(db_base, &db, &migrated)) return 1;
    if (migrated) {
        if (!db_save(db_base, &db)) return 1;
        fprintf(stderr, "note: converted %s to the v2 record format\n", db_base);
    }

    b8 ok = 0;
    if (i >= argc) {
        ok = cmd_verify(&db, threads);
    } else if (strcmp(argv[i], "-set") == 0) {
        const char *cmd = NULL;
        const char **files = ST_arena_push(db.arena, sizeof(*files) * (size_t)(argc + 1));
        u32 nfiles = 0;
        b8 missing = 0;
        for (int k = i + 1; k < argc; k++) {
            if (strcmp(argv[k], "-f") == 0) {
                while (k + 1 < argc && argv[k + 1][0] != '-') {
                    k += 1;
                    const char *resolved = resolve_test_file(db.arena, start_cwd, db_dir, argv[k]);
                    if (!resolved) {
                        fprintf(stderr, "error: %s does not exist\n", argv[k]);
                        missing = 1;
                    } else {
                        files[nfiles++] = resolved;
                    }
                }
            } else if (strcmp(argv[k], "-cmd") == 0 && k + 1 < argc) {
                cmd = argv[++k];
            }
        }
        if ((nfiles == 0 && !missing) || !cmd) usage(argv[0]);
        else if (!missing) ok = cmd_set(&db, db_base, files, nfiles, cmd);
    } else if (strcmp(argv[i], "-help") == 0) {
        help(argv[0]);
        ok = 1;
    } else if (strcmp(argv[i], "-record") == 0) {
        ok = cmd_record(db_base, &db, threads);
    } else if (strcmp(argv[i], "-rerecord") == 0) {
        if (i + 1 < argc && strcmp(argv[i + 1], "-all") == 0) {
            ok = cmd_rerecord(&db, db_base, NULL, NULL, threads);
        } else if (i + 2 < argc && strcmp(argv[i + 1], "-f") == 0) {
            const char *resolved = resolve_test_file(db.arena, start_cwd, db_dir, argv[i + 2]);
            if (!resolved) fprintf(stderr, "error: %s does not exist\n", argv[i + 2]);
            else ok = cmd_rerecord(&db, db_base, resolved, i + 3 < argc ? argv[i + 3] : NULL, threads);
        } else {
            usage(argv[0]);
        }
    } else {
        usage(argv[0]);
    }

    ST_arena_free(db.arena);
    return ok ? 0 : 1;
}
