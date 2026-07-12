#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/wait.h>

#define RECORD_DB_PATH "record.sth"
#define DB_MAGIC "STHREC01"
#define SHA_LEN 64

static const uint8_t pad4[] = { 0x00, 0xFF, 0x00, 0xFF };
static const uint8_t pad8[] = { 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF };

typedef enum
{
    ENTRY_SET,
    ENTRY_RECORDED,
} entry_state_t;

typedef struct
{
    entry_state_t kind;
    char *file_name;
    char *cmd;
    char sha_of_file[SHA_LEN + 1];
    int32_t expected_status;
    char *result;
    uint32_t result_len;
} test_entry_t;

typedef struct
{
    test_entry_t *items;
    uint32_t count;
    uint32_t capacity;
} test_db_t;

static test_entry_t *test_entry_find(test_db_t *db, const char *name)
{
    for (uint32_t i = 0; i < db->count; i++)
    {
        if (strcmp(name, db->items[i].file_name) == 0) return &db->items[i];
    }
    return NULL;
}

static test_entry_t *db_push(test_db_t *db)
{
    if (db->count >= db->capacity)
    {
        db->capacity = db->capacity ? db->capacity * 2 : 16;
        db->items = realloc(db->items, db->capacity * sizeof(*db->items));
    }
    test_entry_t *e = &db->items[db->count++];
    memset(e, 0, sizeof(*e));
    return e;
}

static void db_free(test_db_t *db)
{
    for (uint32_t i = 0; i < db->count; i++)
    {
        free(db->items[i].file_name);
        free(db->items[i].cmd);
        free(db->items[i].result);
    }
    free(db->items);
    memset(db, 0, sizeof(*db));
}

static char *cmd_expand(const char *cmd, const char *file_name)
{
    size_t flen = strlen(file_name);
    size_t cap = strlen(cmd) + 1;
    for (const char *p = cmd; (p = strstr(p, "$file")); p += 5) cap += flen;

    char *out = malloc(cap * sizeof(*out));
    char *w = out;
    const char *r = cmd;
    while (*r)
    {
        if (r[0] == '$' && strncmp(r, "$file", 5) == 0)
        {
            memcpy(w, file_name, flen);
            w += flen;
            r += 5;
        }
        else *w++ = *r++;
    }
    *w = 0;
    return out;
}

static uint32_t codec_state;

static void codec_reset(void)
{
    codec_state = 0x53544831;
}

static uint8_t codec_next(void)
{
    codec_state ^= codec_state << 13;
    codec_state ^= codec_state >> 17;
    codec_state ^= codec_state << 5;
    return (uint8_t)codec_state;
}

static bool write_bytes(FILE *f, const void *p, size_t n)
{
    const uint8_t *src = p;
    uint8_t buf[4096];
    while (n > 0)
    {
        size_t chunk = n < sizeof(buf) ? n : sizeof(buf);
        for (size_t i = 0; i < chunk; i++)
        {
            buf[i] = src[i] ^ codec_next();
        }
        if (fwrite(buf, 1, chunk, f) != chunk) return false;
        src += chunk;
        n -= chunk;
    }
    return true;
}

static bool write_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return write_bytes(f, b, sizeof(b));
}

static bool write_str(FILE *f, const char *s, uint32_t len)
{
    return write_u32(f, len) && write_bytes(f, s, len);
}

static bool read_bytes(FILE *f, void *p, size_t n)
{
    if (fread(p, 1, n, f) != n) return false;
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
    {
        b[i] ^= codec_next();
    }
    return true;
}

static bool read_u32(FILE *f, uint32_t *v)
{
    uint8_t b[4];
    if (!read_bytes(f, b, sizeof(b))) return false;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return true;
}

static char *read_str(FILE *f, uint32_t *out_len)
{
    uint32_t len;
    if (!read_u32(f, &len)) return NULL;
    char *s = malloc(len + 1);
    if (!s) return NULL;
    if (!read_bytes(f, s, len))
    {
        free(s);
        return NULL;
    }
    s[len] = 0;
    if (out_len) *out_len = len;
    return s;
}

static bool read_pad(FILE *f, const uint8_t *pad, size_t n)
{
    uint8_t b[8];
    return read_bytes(f, b, n) && memcmp(b, pad, n) == 0;
}

static bool db_load(test_db_t *db)
{
    memset(db, 0, sizeof(*db));

    FILE *f = fopen(RECORD_DB_PATH, "rb");
    if (!f) return true;

    char magic[8];
    uint32_t count;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, DB_MAGIC, 8) != 0)
    {
        fprintf(stderr, "error: %s is corrupt or not a record db\n", RECORD_DB_PATH);
        fclose(f);
        return false;
    }
    codec_reset();
    if (!read_u32(f, &count))
    {
        fprintf(stderr, "error: %s is corrupt or not a record db\n", RECORD_DB_PATH);
        fclose(f);
        return false;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        test_entry_t *e = db_push(db);
        uint8_t kind;
        bool ok = read_bytes(f, &kind, 1)
               && (e->file_name = read_str(f, NULL)) != NULL
               && read_pad(f, pad8, 8)
               && (e->cmd = read_str(f, NULL)) != NULL
               && read_pad(f, pad4, 4)
               && read_bytes(f, e->sha_of_file, SHA_LEN + 1)
               && read_pad(f, pad4, 4)
               && read_u32(f, (uint32_t *)&e->expected_status)
               && read_pad(f, pad4, 4)
               && (e->result = read_str(f, &e->result_len)) != NULL
               && read_pad(f, pad8, 8);
        if (!ok)
        {
            fprintf(stderr, "error: %s truncated at entry %u\n", RECORD_DB_PATH, i);
            fclose(f);
            return false;
        }
        e->kind = kind;
    }
    fclose(f);
    return true;
}

static bool db_save(test_db_t *db)
{
    FILE *f = fopen(RECORD_DB_PATH, "wb");
    if (!f)
    {
        fprintf(stderr, "error: could not open %s for writing\n", RECORD_DB_PATH);
        return false;
    }

    bool ok = fwrite(DB_MAGIC, 1, 8, f) == 8;
    codec_reset();
    ok = ok && write_u32(f, db->count);
    for (uint32_t i = 0; ok && i < db->count; i++)
    {
        test_entry_t *e = &db->items[i];
        uint8_t kind = (uint8_t)e->kind;
        ok = write_bytes(f, &kind, 1)
          && write_str(f, e->file_name, strlen(e->file_name))
          && write_bytes(f, pad8, 8)
          && write_str(f, e->cmd, strlen(e->cmd))
          && write_bytes(f, pad4, 4)
          && write_bytes(f, e->sha_of_file, SHA_LEN + 1)
          && write_bytes(f, pad4, 4)
          && write_u32(f, (uint32_t)e->expected_status)
          && write_bytes(f, pad4, 4)
          && write_str(f, e->result, e->result_len)
          && write_bytes(f, pad8, 8);
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) fprintf(stderr, "error: failed writing %s\n", RECORD_DB_PATH);
    return ok;
}

static bool sha256_of(const char *file_path, char out[SHA_LEN + 1])
{
    char sha_cmd[512];
    snprintf(sha_cmd, sizeof(sha_cmd), "sha256sum '%s'", file_path);

    FILE *p = popen(sha_cmd, "r");
    if (!p) return false;

    size_t got = fread(out, 1, SHA_LEN, p);
    out[got] = 0;
    int rc = pclose(p);
    return rc == 0 && got == SHA_LEN;
}

static int32_t run_cmd(const char *cmd, char **out, uint32_t *out_len)
{
    char full[1024];
    snprintf(full, sizeof(full), "(%s) 2>&1", cmd);

    FILE *p = popen(full, "r");
    if (!p) return -1;

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    size_t got;
    while ((got = fread(buf + len, 1, cap - len - 1, p)) > 0)
    {
        len += got;
        if (len + 1 >= cap)
        {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    buf[len] = 0;

    int rc = pclose(p);
    *out = buf;
    *out_len = (uint32_t)len;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void exe_path_of(const char *file_name, char *out, size_t n)
{
    if (strchr(file_name, '/')) snprintf(out, n, "%s.exe", file_name);
    else snprintf(out, n, "./%s.exe", file_name);
}

static bool build_entry(test_entry_t *e)
{
    char *cmd = cmd_expand(e->cmd, e->file_name);
    char *build_out;
    uint32_t build_len;
    int32_t status = run_cmd(cmd, &build_out, &build_len);
    if (status != 0)
    {
        fprintf(stderr, "  [FAIL] %s: build failed (status %d): `%s`\n%s",
                e->file_name, status, cmd, build_out);
        free(cmd);
        free(build_out);
        return false;
    }
    free(cmd);
    free(build_out);
    return true;
}

static int32_t run_exe(const char *file_name, char **out, uint32_t *out_len)
{
    char exe[512];
    exe_path_of(file_name, exe, sizeof(exe));
    int32_t status = run_cmd(exe, out, out_len);
    remove(exe);
    return status;
}

static void print_diff(const char *expected, const char *got)
{
    uint32_t line = 1;
    while (*expected || *got)
    {
        size_t elen = strcspn(expected, "\n");
        size_t glen = strcspn(got, "\n");
        if (elen != glen || memcmp(expected, got, elen) != 0)
        {
            printf("       line %u:\n", line);
            printf("       - %.*s\n", (int)elen, expected);
            printf("       + %.*s\n", (int)glen, got);
        }
        expected += elen + (expected[elen] == '\n');
        got += glen + (got[glen] == '\n');
        line++;
    }
}

static bool record_file(test_entry_t *e)
{
    if (!sha256_of(e->file_name, e->sha_of_file))
    {
        fprintf(stderr, "  [FAIL] %s: could not hash file\n", e->file_name);
        return false;
    }

    if (!build_entry(e)) return false;

    free(e->result);
    e->expected_status = run_exe(e->file_name, &e->result, &e->result_len);
    printf("  [REC ] %s -> status %d, %u bytes of output\n",
           e->file_name, e->expected_status, e->result_len);

    e->kind = ENTRY_RECORDED;
    return true;
}

static bool cmd_set(test_db_t *db, const char *file_name, const char *cmd)
{
    test_entry_t *e = test_entry_find(db, file_name);
    if (!e)
    {
        e = db_push(db);
        e->file_name = strdup(file_name);
        e->result = strdup("");
        e->result_len = 0;
    }
    else
    {
        free(e->cmd);
    }
    e->cmd = strdup(cmd);
    printf("set: %s -> `%s`\n", file_name, cmd);
    return db_save(db);
}

static bool cmd_record(test_db_t *db)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < db->count; i++)
    {
        if (db->items[i].kind == ENTRY_SET)
        {
            record_file(&db->items[i]);
            n++;
        }
    }
    if (n == 0)
    {
        printf("record: nothing to do (no set-but-unrecorded files)\n");
        return true;
    }
    return db_save(db);
}

static bool cmd_rerecord(test_db_t *db, const char *file_name, const char *new_cmd)
{
    if (file_name)
    {
        test_entry_t *e = test_entry_find(db, file_name);
        if (!e)
        {
            fprintf(stderr, "error: no test for %s, use -set first\n", file_name);
            return false;
        }
        if (new_cmd)
        {
            free(e->cmd);
            e->cmd = strdup(new_cmd);
        }
        record_file(e);
    }
    else
    {
        for (uint32_t i = 0; i < db->count; i++) record_file(&db->items[i]);
    }
    return db_save(db);
}

static bool cmd_verify(test_db_t *db)
{
    uint32_t ran = 0;
    uint32_t failed = 0;
    for (uint32_t i = 0; i < db->count; i++)
    {
        test_entry_t *e = &db->items[i];
        if (e->kind != ENTRY_RECORDED) continue;
        ran++;

        char sha[SHA_LEN + 1];
        if (!sha256_of(e->file_name, sha) || strcmp(sha, e->sha_of_file) != 0)
        {
            printf("[FAIL] %s: file hash mismatch (file modified or missing), rerecord it\n",
                   e->file_name);
            failed++;
            continue;
        }

        if (!build_entry(e))
        {
            failed++;
            continue;
        }

        char *out;
        uint32_t out_len;
        int32_t status = run_exe(e->file_name, &out, &out_len);

        bool status_ok = status == e->expected_status;
        bool output_ok = out_len == e->result_len && memcmp(out, e->result, out_len) == 0;

        if (status_ok && output_ok) printf("[ OK ] %s\n", e->file_name);
        else
        {
            failed++;
            printf("[FAIL] %s\n", e->file_name);
            if (!status_ok)
            {
                printf("       status: expected %d, got %d\n", e->expected_status, status);
            }
            if (!output_ok)
            {
                printf("       output mismatch (expected %u bytes, got %u bytes):\n",
                       e->result_len, out_len);
                print_diff(e->result, out);
            }
        }
        free(out);
    }
    printf("%u ran, %u failed\n", ran, failed);
    return failed == 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage:\n"
        "  %s -set -f <file> -cmd \"<cmd with $file>\"\n"
        "  %s -record\n"
        "  %s -rerecord -f <file> [cmd]\n"
        "  %s -rerecord -all\n"
        "  %s -help\n"
        "  %s\n",
        prog, prog, prog, prog, prog, prog);
}

static void help(const char *prog)
{
    printf(
        "%s - snapshot test framework\n"
        "\n"
        "  %s\n"
        "      Verify all recorded tests: check the file hash, build, run the\n"
        "      exe, and report any mismatch in exit status or output.\n"
        "\n"
        "  %s -set -f <file> -cmd '<build cmd>'\n"
        "      Register <file> with a build command. $file in the cmd expands\n"
        "      to <file> at run time (use single quotes so the shell does not\n"
        "      expand it). The cmd must produce <file>.exe.\n"
        "\n"
        "  %s -record\n"
        "      Build and run every set-but-unrecorded file, snapshot its exit\n"
        "      status and stdout+stderr, and store the file hash.\n"
        "\n"
        "  %s -rerecord -f <file> [cmd]\n"
        "      Rerecord one test. If cmd is given it replaces the set cmd.\n"
        "\n"
        "  %s -rerecord -all\n"
        "      Rerecord every test, keeping file names and cmds.\n"
        "\n"
        "  %s -help\n"
        "      Show this message.\n"
        "\n"
        "The exe is deleted after every run. Snapshots live in %s.\n",
        prog, prog, prog, prog, prog, prog, prog, RECORD_DB_PATH);
}

int main(int argc, char *argv[])
{
    test_db_t db;
    if (!db_load(&db)) return 1;

    bool ok = false;

    if (argc == 1) ok = cmd_verify(&db);

    else if (strcmp(argv[1], "-set") == 0)
    {
        const char *file = NULL;
        const char *cmd = NULL;
        for (int i = 2; i < argc - 1; i++)
        {
            if (strcmp(argv[i], "-f") == 0) file = argv[++i];
            if (strcmp(argv[i], "-cmd") == 0) cmd = argv[++i];
        }
        if (!file || !cmd) usage(argv[0]);
        else ok = cmd_set(&db, file, cmd);
    }
    else if (strcmp(argv[1], "-help") == 0)
    {
        help(argv[0]);
        ok = true;
    }
    else if (strcmp(argv[1], "-record") == 0) ok = cmd_record(&db);
    else if (strcmp(argv[1], "-rerecord") == 0)
    {
        if (argc >= 3 && strcmp(argv[2], "-all") == 0)
            ok = cmd_rerecord(&db, NULL, NULL);

        else if (argc >= 4 && strcmp(argv[2], "-f") == 0)
            ok = cmd_rerecord(&db, argv[3], argc >= 5 ? argv[4] : NULL);

        else usage(argv[0]);
    }
    else usage(argv[0]);

    db_free(&db);
    return ok ? 0 : 1;
}
