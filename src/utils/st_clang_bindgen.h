#ifndef STBIND_H
#define STBIND_H

int stbind_generate(const char *header_path, const char *output_path,
                    const char **extra_args, int n_extra_args,
                    char **err_msg);

char *stbind_generate_string(const char *header_path,
                             const char **extra_args, int n_extra_args,
                             char **err_msg);

#endif /* STBIND_H */

#ifdef STBIND_IMPLEMENTATION

#include <clang-c/Index.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdint.h>

/*
 * Compile-time switches (define before including with STBIND_IMPLEMENTATION).
 */
#ifndef STBIND_STRIP_ENUM_PREFIX
#define STBIND_STRIP_ENUM_PREFIX 1   /* SDL_KEY_A, SDL_KEY_B -> enum { A, B } */
#endif
#ifndef STBIND_EMIT_MACROS
#define STBIND_EMIT_MACROS 1         /* #define N 5  ->  N :: 5; */
#endif
#ifndef STBIND_EMIT_DOCS
#define STBIND_EMIT_DOCS 1           /* copy header doc comments as // lines */
#endif
#ifndef STBIND_EMIT_EXTERN_VARS
#define STBIND_EMIT_EXTERN_VARS 1    /* extern int x;  ->  extern x: i32; */
#endif
#ifndef STBIND_DETECT_ENUM_FLAGS
#define STBIND_DETECT_ENUM_FLAGS 1   /* 1,2,4,8 enums become enum_flag */
#endif
#ifndef STBIND_PUB
#define STBIND_PUB 1                 /* prefix every top-level decl with pub */
#endif
#ifndef STBIND_DEEP_DEFAULT
#define STBIND_DEEP_DEFAULT 1        /* also bind headers pulled in by the input header */
#endif
#ifndef STBIND_UNION_DIRECTIVE
#define STBIND_UNION_DIRECTIVE " #union"
#endif
#ifndef STBIND_PACK_DIRECTIVE
#define STBIND_PACK_DIRECTIVE " #pack"
#endif

typedef struct stbind_arena_chunk_t stbind_arena_chunk_t;
struct stbind_arena_chunk_t {
    stbind_arena_chunk_t *next;
    size_t pos, cap;
    char data[];
};

typedef struct {
    stbind_arena_chunk_t *head;
    stbind_arena_chunk_t *cur;
} stbind_arena_t;

#define STBIND_ARENA_CHUNK_SIZE (64 * 1024)
#define STBIND_ARENA_ALIGN 16

static stbind_arena_t g_arena;

static stbind_arena_chunk_t *stbind_arena_chunk_alloc(size_t capacity) {
    stbind_arena_chunk_t *chunk = calloc(1, sizeof(*chunk) + capacity);
    chunk->next = NULL;
    chunk->pos = 0;
    chunk->cap = capacity;
    return chunk;
}

static void *stbind_arena_push(size_t size) {
    if (!g_arena.head) {
        g_arena.head = stbind_arena_chunk_alloc(STBIND_ARENA_CHUNK_SIZE);
        g_arena.cur = g_arena.head;
    }
    if (size == 0)
        return g_arena.cur->data + g_arena.cur->pos;
    size_t pos = (g_arena.cur->pos + (STBIND_ARENA_ALIGN - 1)) & ~(size_t)(STBIND_ARENA_ALIGN - 1);
    if (pos + size > g_arena.cur->cap) {
        size_t cap = size > STBIND_ARENA_CHUNK_SIZE ? size : STBIND_ARENA_CHUNK_SIZE;
        stbind_arena_chunk_t *next = stbind_arena_chunk_alloc(cap);
        g_arena.cur->next = next;
        g_arena.cur = next;
        pos = 0;
    }
    g_arena.cur->pos = pos + size;
    return g_arena.cur->data + pos;
}

static char *stbind_arena_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = stbind_arena_push(n);
    memcpy(p, s, n);
    return p;
}

static void stbind_arena_reset(void) {
    if (!g_arena.head)
        return;
    stbind_arena_chunk_t *chunk = g_arena.head->next;
    while (chunk) {
        stbind_arena_chunk_t *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    g_arena.head->next = NULL;
    g_arena.head->pos = 0;
    g_arena.cur = g_arena.head;
}

typedef struct {
    char *data;
    size_t len, cap;
} sb_t;

static void sb_init(sb_t *sb) {
    sb->cap = 256;
    sb->len = 0;
    sb->data = malloc(sb->cap);
    sb->data[0] = 0;
}

static void sb_ensure(sb_t *sb, size_t extra) {
    if (!sb->cap) {
        sb->cap = 256;
        sb->data = malloc(sb->cap);
        sb->data[0] = 0;
    }
    if (sb->len + extra + 1 > sb->cap) {
        while (sb->len + extra + 1 > sb->cap)
            sb->cap *= 2;
        sb->data = realloc(sb->data, sb->cap);
    }
}

static void sb_append(sb_t *sb, const char *s) {
    size_t n = strlen(s);
    sb_ensure(sb, n);
    memcpy(sb->data + sb->len, s, n + 1);
    sb->len += n;
}

static void sb_appendf(sb_t *sb, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    sb_ensure(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

static void sb_indent(sb_t *sb, int depth) {
    for (int i = 0; i < depth; i++)
        sb_append(sb, "    ");
}

static char *cxstr(CXString s) {
    const char *c = clang_getCString(s);
    char *out = stbind_arena_strdup(c ? c : "");
    clang_disposeString(s);
    return out;
}

static const char *const g_reserved[] = {
    "fn", "struct", "enum", "extern", "pub", "null", "if", "else", "while", "for",
    "cast", "type_of", "type_info", "sizeof", "and", "or", "not", "default",
    "enum_flag", "union", "case", "return", "using", "break", "continue", "defer",
    "true", "false", "const", "static", "label", "goto", "impl",
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "f128",
    "char", "void", "any", "bool", "string", NULL
};

static int is_reserved(const char *s) {
    for (int i = 0; g_reserved[i]; i++)
        if (!strcmp(s, g_reserved[i]))
            return 1;
    return 0;
}

/* Escape C identifiers that collide with storth keywords / primitive names. */
static const char *safe_ident(const char *s) {
    if (!is_reserved(s))
        return s;
    size_t n = strlen(s);
    char *p = stbind_arena_push(n + 2);
    memcpy(p, s, n);
    p[n] = '_';
    p[n + 1] = 0;
    return p;
}

static int decl_is_anonymous(CXCursor d);

static sb_t g_fail;
static int g_fail_set;

static void fail_reset(void) {
    g_fail.len = 0;
    if (g_fail.data)
        g_fail.data[0] = 0;
    else
        sb_init(&g_fail);
    g_fail_set = 0;
}

static void note_fail(CXCursor c, const char *why) {
    if (g_fail_set)
        return;
    g_fail_set = 1;
    CXSourceLocation loc = clang_getCursorLocation(c);
    CXFile file;
    unsigned line = 0, col = 0;
    clang_getExpansionLocation(loc, &file, &line, &col, NULL);
    char *fname = cxstr(clang_getFileName(file));
    if (why) {
        sb_appendf(&g_fail, "%s at %s:%u:%u", why, fname, line, col);
    } else {
        char *kind = cxstr(clang_getCursorKindSpelling(clang_getCursorKind(c)));
        sb_appendf(&g_fail, "unsupported %s at %s:%u:%u", kind, fname, line, col);
    }
}

typedef struct {
    CXCursor *items;
    unsigned count, cap;
} cursors_t;

static void cursors_push(cursors_t *c, CXCursor cur) {
    if (c->count >= c->cap) {
        unsigned new_cap = c->cap ? c->cap * 2 : 4;
        CXCursor *new_items = stbind_arena_push(sizeof(*c->items) * new_cap);
        if (c->count)
            memcpy(new_items, c->items, sizeof(*c->items) * c->count);
        c->items = new_items;
        c->cap = new_cap;
    }
    c->items[c->count++] = cur;
}

static enum CXChildVisitResult collect_children_cb(CXCursor c, CXCursor parent, CXClientData data) {
    (void)parent;
    cursors_push((cursors_t *)data, c);
    return CXChildVisit_Continue;
}

static cursors_t get_children(CXCursor cur) {
    cursors_t out = {0};
    clang_visitChildren(cur, collect_children_cb, &out);
    return out;
}

static void free_children(cursors_t *c) {
    c->items = NULL;
    c->count = c->cap = 0;
}

static int map_type(CXType t, sb_t *out);

static int g_used_complex32;
static int g_used_complex64;

static const char *complex_name_for(CXType complex_ty) {
    CXType elem = clang_getElementType(complex_ty);
    if (elem.kind == CXType_Float)
        return "Complex32";
    if (elem.kind == CXType_Double)
        return "Complex64";
    return NULL;
}

static const char *complex_prefix_for(CXType complex_ty) {
    CXType elem = clang_getElementType(complex_ty);
    if (elem.kind == CXType_Float)
        return "complex32";
    if (elem.kind == CXType_Double)
        return "complex64";
    return NULL;
}

static void mark_complex_used(const char *prefix) {
    if (!strcmp(prefix, "complex32"))
        g_used_complex32 = 1;
    else
        g_used_complex64 = 1;
}

typedef struct {
    const char *key;
    CXCursor cursor;
    long long ival;
    int used;
} hset_entry_t;

typedef struct {
    hset_entry_t *slots;
    unsigned cap, count;
} hset_t;

static unsigned hash_str(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static hset_entry_t *hset_find_slot(hset_entry_t *slots, unsigned cap, const char *key) {
    unsigned i = hash_str(key) & (cap - 1);
    for (;;) {
        if (!slots[i].used || !strcmp(slots[i].key, key))
            return &slots[i];
        i = (i + 1) & (cap - 1);
    }
}

static void hset_grow(hset_t *h) {
    unsigned new_cap = h->cap ? h->cap * 2 : 64;
    hset_entry_t *new_slots = stbind_arena_push(sizeof(*new_slots) * new_cap);
    memset(new_slots, 0, sizeof(*new_slots) * new_cap);
    for (unsigned i = 0; i < h->cap; i++) {
        if (h->slots[i].used) {
            hset_entry_t *slot = hset_find_slot(new_slots, new_cap, h->slots[i].key);
            *slot = h->slots[i];
        }
    }
    h->slots = new_slots;
    h->cap = new_cap;
}

static int hset_add(hset_t *h, const char *key, CXCursor cursor) {
    if (!h->cap || h->count * 4 >= h->cap * 3)
        hset_grow(h);
    hset_entry_t *slot = hset_find_slot(h->slots, h->cap, key);
    if (slot->used)
        return 0;
    slot->key = stbind_arena_strdup(key);
    slot->cursor = cursor;
    slot->used = 1;
    h->count++;
    return 1;
}

static int hset_has(hset_t *h, const char *key) {
    if (!h->cap) return 0;
    return hset_find_slot(h->slots, h->cap, key)->used;
}

static CXCursor *hset_find(hset_t *h, const char *key) {
    if (!h->cap) return NULL;
    hset_entry_t *slot = hset_find_slot(h->slots, h->cap, key);
    return slot->used ? &slot->cursor : NULL;
}

static hset_t g_needed_types;
static hset_t g_opaque_types;
static hset_t g_emitted_types;
static hset_t g_record_index;
static hset_t g_consts;       /* name -> integer value (enum constants, macros) */
static hset_t g_macro_names;
static hset_t g_typedef_index; /* typedef-of-anonymous-record/enum name -> typedef cursor */
static hset_t g_scope_cache;   /* file path -> in scope? */
static int g_deep;
static const char *g_incl[32];
static const char *g_excl[32];
static int g_n_incl, g_n_excl;

static void const_add(const char *name, long long v) {
    hset_add(&g_consts, name, clang_getNullCursor());
    hset_find_slot(g_consts.slots, g_consts.cap, name)->ival = v;
}

static int const_get(const char *name, long long *v) {
    if (!g_consts.cap)
        return 0;
    hset_entry_t *e = hset_find_slot(g_consts.slots, g_consts.cap, name);
    if (!e->used)
        return 0;
    *v = e->ival;
    return 1;
}

static int map_pointee_or_fn(CXType pointee, sb_t *out) {
    if (pointee.kind == CXType_FunctionProto || pointee.kind == CXType_FunctionNoProto) {
        sb_append(out, "*fn(");
        int n = clang_getNumArgTypes(pointee);
        for (int i = 0; i < n; i++) {
            if (i)
                sb_append(out, ", ");
            sb_appendf(out, "a%d: ", i);
            if (!map_type(clang_getArgType(pointee, i), out))
                return 0;
        }
        sb_append(out, ") -> ");
        CXType ret = clang_getResultType(pointee);
        if (ret.kind == CXType_Void)
            sb_append(out, "void");
        else if (!map_type(ret, out))
            return 0;
        return 1;
    }
    sb_append(out, "*");
    return map_type(pointee, out);
}

static char *cxstr_unconst(CXString s) {
    char *str = cxstr(s);
    if (!str)
	return str;

    const char *const_decl = "const ";
    if (!strncmp(str, const_decl, strlen(const_decl))) {
	return strdup(str + strlen(const_decl));
    }
    return strdup(str);
}

static int is_va_list_tag_type(CXType t) {
    t = clang_getCanonicalType(t);

    for (;;) {
        if (t.kind == CXType_ConstantArray ||
            t.kind == CXType_IncompleteArray) {
            t = clang_getArrayElementType(t);
            t = clang_getCanonicalType(t);
            continue;
        }

        if (t.kind == CXType_Typedef) {
            t = clang_getTypedefDeclUnderlyingType(
                clang_getTypeDeclaration(t));
            t = clang_getCanonicalType(t);
            continue;
        }

        if (t.kind == CXType_Elaborated) {
            t = clang_Type_getNamedType(t);
            t = clang_getCanonicalType(t);
            continue;
        }

        break;
    }

    if (t.kind != CXType_Record)
        return 0;

    CXCursor decl = clang_getTypeDeclaration(t);
    char *name = cxstr(clang_getCursorSpelling(decl));

    int result = !strcmp(name, "__va_list_tag");
    return result;
}


static int map_type(CXType t, sb_t *out) {
    if (is_va_list_tag_type(t)) {
        sb_append(out, "*void");
        return 1;
    }

    if (t.kind == CXType_Typedef) {
        CXType canon = clang_getCanonicalType(t);
        if (canon.kind == CXType_Record || canon.kind == CXType_Enum) {
	    char *name = cxstr_unconst(clang_getTypeSpelling(t));
	    if (canon.kind == CXType_Record &&
		!clang_isCursorDefinition(clang_getTypeDeclaration(canon))) {
		hset_add(&g_opaque_types, name, clang_getNullCursor());
	    } else {
		hset_add(&g_needed_types, name, clang_getNullCursor());
	    }
	    sb_append(out, name);
	    free(name);
	    return 1;
	}
	return map_type(canon, out);
    }
    if (t.kind == CXType_Elaborated) {
        CXType named = clang_Type_getNamedType(t);
        if (named.kind != CXType_Invalid && named.kind != CXType_Elaborated)
            return map_type(named, out);
        return map_type(clang_getCanonicalType(t), out);
    }

    switch (t.kind) {
        case CXType_Void: sb_append(out, "void"); return 1;
        case CXType_Bool: sb_append(out, "bool"); return 1;
        case CXType_Char_S:
        case CXType_Char_U:
        case CXType_SChar: sb_append(out, "char"); return 1;
        case CXType_UChar: sb_append(out, "u8"); return 1;
        case CXType_Short: sb_append(out, "i16"); return 1;
        case CXType_UShort: sb_append(out, "u16"); return 1;
        case CXType_Int: sb_append(out, "i32"); return 1;
        case CXType_UInt: sb_append(out, "u32"); return 1;
        case CXType_Long: sb_append(out, "i64"); return 1;
        case CXType_ULong: sb_append(out, "u64"); return 1;
        case CXType_LongLong: sb_append(out, "i64"); return 1;
        case CXType_ULongLong: sb_append(out, "u64"); return 1;
        case CXType_Float: sb_append(out, "f32"); return 1;
        case CXType_Double: sb_append(out, "f64"); return 1;
        case CXType_LongDouble: sb_append(out, "f128"); return 1;
        case CXType_Pointer:
            return map_pointee_or_fn(clang_getPointeeType(t), out);
        case CXType_ConstantArray: {
            long long n = clang_getArraySize(t);
            sb_appendf(out, "[%lld]", n);
            return map_type(clang_getArrayElementType(t), out);
        }
        case CXType_IncompleteArray:
            sb_append(out, "*");
            return map_type(clang_getArrayElementType(t), out);
        case CXType_Complex: {
            const char *nm = complex_name_for(t);
            if (!nm)
                return 0;
            mark_complex_used(complex_prefix_for(t));
            sb_append(out, nm);
            return 1;
        }
        case CXType_Enum: {
            char *name = cxstr(clang_getTypeSpelling(t));
            if (!name[0] || strstr(name, "unnamed") || strstr(name, "anonymous")) {
                /* anonymous enum used as a field/param type: it is just its integer type */
                CXType it = clang_getEnumDeclIntegerType(clang_getTypeDeclaration(t));
                if (it.kind != CXType_Invalid && it.kind != CXType_Enum)
                    return map_type(it, out);
                if (!g_fail_set) {
                    g_fail_set = 1;
                    sb_appendf(&g_fail, "reference to an anonymous %s type ('%s')",
                              t.kind == CXType_Enum ? "enum" : "struct", name);
                }
                return 0;
            }
            const char *n = name;
            if (!strncmp(n, "struct ", 7)) n += 7;
            else if (!strncmp(n, "enum ", 5)) n += 5;
            else if (!strncmp(n, "union ", 6)) {
                if (!g_fail_set) {
                    g_fail_set = 1;
                    sb_appendf(&g_fail, "reference to union type '%s' (unions unsupported)", name);
                }
                return 0;
            }

            sb_append(out, n);
            hset_add(&g_needed_types, n, clang_getNullCursor());
            return 1;
        }
        case CXType_Record: {
            char *name = cxstr(clang_getTypeSpelling(t));
            if (!name[0] || strstr(name, "unnamed") || strstr(name, "anonymous")) {
                if (!g_fail_set) {
                    g_fail_set = 1;
                    sb_appendf(&g_fail, "reference to an anonymous struct type ('%s')", name);
                }
                return 0;
            }
            const char *n = name;
            if (!strncmp(n, "struct ", 7)) n += 7;
            else if (!strncmp(n, "union ", 6)) n += 6;

            sb_append(out, n);
            if (!clang_isCursorDefinition(clang_getTypeDeclaration(t)))
                hset_add(&g_opaque_types, n, clang_getNullCursor());
            else
                hset_add(&g_needed_types, n, clang_getNullCursor());
            return 1;
        }
        default: {
            if (!g_fail_set) {
                g_fail_set = 1;
                char *ts = cxstr(clang_getTypeSpelling(t));
                sb_appendf(&g_fail, "unsupported type '%s' (kind %d)", ts, (int)t.kind);
            }
            return 0;
        }
    }
}

static char *type_str(CXType t);

/* glibc's __SOCKADDR_ARG & friends: a union of pointers is a transparent_union. */
static int union_is_all_pointers(CXCursor decl) {
    cursors_t ch = get_children(decl);
    int fields = 0, ok = 1;
    for (unsigned i = 0; i < ch.count; i++) {
        if (clang_getCursorKind(ch.items[i]) != CXCursor_FieldDecl)
            continue;
        fields++;
        if (clang_getCanonicalType(clang_getCursorType(ch.items[i])).kind != CXType_Pointer)
            ok = 0;
    }
    free_children(&ch);
    return ok && fields >= 2;
}

static char *param_type_str(CXType t) {
    CXType canon = clang_getCanonicalType(t);
    if (canon.kind == CXType_Record) {
        CXCursor rd = clang_getTypeDeclaration(canon);
        if (clang_getCursorKind(rd) == CXCursor_UnionDecl && union_is_all_pointers(rd))
            return "*void";
    }
    if (canon.kind == CXType_ConstantArray || canon.kind == CXType_IncompleteArray) {
        sb_t sb;
        sb_init(&sb);
        sb_append(&sb, "*");
        if (!map_type(clang_getArrayElementType(canon), &sb)) {
            free(sb.data);
            return NULL;
        }
        char *result = stbind_arena_strdup(sb.data);
        free(sb.data);
        return result;
    }
    return type_str(t);
}

static char *type_str(CXType t) {
    sb_t sb;
    sb_init(&sb);
    if (!map_type(t, &sb)) {
        free(sb.data);
        return NULL;
    }
    char *result = stbind_arena_strdup(sb.data);
    free(sb.data);
    return result;
}

static CXCursor skip_transparent(CXCursor cur) {
    for (;;) {
        enum CXCursorKind k = clang_getCursorKind(cur);
        if (k != CXCursor_UnexposedExpr && k != CXCursor_ParenExpr)
            return cur;
        cursors_t ch = get_children(cur);
        if (ch.count != 1) {
            free_children(&ch);
            return cur;
        }
        CXCursor next = ch.items[0];
        free_children(&ch);
        cur = next;
    }
}

/*
 * Length of the prefix shared by every enumerator of an enum, cut at an
 * underscore boundary (SDL_KEY_A, SDL_KEY_B -> "SDL_KEY_").  0 = do not strip.
 */
static int enum_strip_len(CXCursor d) {
#if STBIND_STRIP_ENUM_PREFIX
    cursors_t ch = get_children(d);
    const char *first = NULL;
    size_t plen = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < ch.count; i++) {
        if (clang_getCursorKind(ch.items[i]) != CXCursor_EnumConstantDecl)
            continue;
        char *nm = cxstr(clang_getCursorSpelling(ch.items[i]));
        if (!n) {
            first = nm;
            plen = strlen(nm);
        } else {
            size_t j = 0;
            while (j < plen && nm[j] == first[j])
                j++;
            plen = j;
        }
        n++;
    }
    int result = 0;
    if (n >= 2) {
        while (plen > 0 && first[plen - 1] != '_')
            plen--;
        result = (int)plen;
        for (unsigned i = 0; result && i < ch.count; i++) {
            if (clang_getCursorKind(ch.items[i]) != CXCursor_EnumConstantDecl)
                continue;
            char *nm = cxstr(clang_getCursorSpelling(ch.items[i]));
            const char *rest = nm + plen;
            if (!rest[0] || isdigit((unsigned char)rest[0]))
                result = 0;
        }
    }
    free_children(&ch);
    return result;
#else
    (void)d;
    return 0;
#endif
}

/* `RED` referenced in a transpiled body becomes `Color.RED` (storth enum access). */
static int emit_enum_const_ref(CXCursor ref, sb_t *out) {
    CXCursor decl = clang_getCursorReferenced(ref);
    if (clang_getCursorKind(decl) != CXCursor_EnumConstantDecl)
        return 0;
    CXCursor en = clang_getCursorSemanticParent(decl);
    char *ename = cxstr(clang_getCursorSpelling(en));
    if (!ename[0] || decl_is_anonymous(en))
        return 0;
    char *vname = cxstr(clang_getCursorSpelling(decl));
    int strip = enum_strip_len(en);
    sb_appendf(out, "%s.%s", ename, safe_ident(vname + strip));
    return 1;
}

static int emit_expr(CXCursor e, sb_t *out);
static int emit_stmt(CXCursor s, sb_t *out, int depth);

static sb_t g_prelude;
static int g_tmp_n;
static int g_cur_depth;

static void prelude_reset(void) {
    g_prelude.len = 0;
    g_prelude.data[0] = 0;
}

static int emit_ternary(CXCursor e, sb_t *out) {
    int depth = g_cur_depth;
    cursors_t ch = get_children(e);
    if (ch.count != 3) {
        note_fail(e, "unsupported ternary form (GNU 'a ?: b' shorthand)");
        free_children(&ch);
        return 0;
    }
    char *ty = type_str(clang_getCursorType(e));
    if (!ty) {
        free_children(&ch);
        return 0;
    }

    char name[32];
    snprintf(name, sizeof(name), "_tern%d", g_tmp_n++);

    sb_indent(&g_prelude, depth);
    sb_appendf(&g_prelude, "%s: %s;\n", name, ty);

    sb_indent(&g_prelude, depth);
    sb_append(&g_prelude, "if ");
    int ok = emit_expr(ch.items[0], &g_prelude);
    sb_append(&g_prelude, " {\n");
    sb_indent(&g_prelude, depth + 1);
    sb_appendf(&g_prelude, "%s = ", name);
    ok = ok && emit_expr(ch.items[1], &g_prelude);
    sb_append(&g_prelude, ";\n");
    sb_indent(&g_prelude, depth);
    sb_append(&g_prelude, "} else {\n");
    sb_indent(&g_prelude, depth + 1);
    sb_appendf(&g_prelude, "%s = ", name);
    ok = ok && emit_expr(ch.items[2], &g_prelude);
    sb_append(&g_prelude, ";\n");
    sb_indent(&g_prelude, depth);
    sb_append(&g_prelude, "}\n");

    free_children(&ch);
    if (!ok)
        return 0;
    sb_append(out, name);
    return 1;
}

static int emit_call(CXCursor e, sb_t *out) {
    cursors_t ch = get_children(e);
    if (ch.count == 0) {
        free_children(&ch);
        return 0;
    }
    CXCursor callee = skip_transparent(ch.items[0]);
    if (clang_getCursorKind(callee) == CXCursor_DeclRefExpr && ch.count == 2) {
        char *cname = cxstr(clang_getCursorSpelling(callee));
        int is_real = !strcmp(cname, "crealf") || !strcmp(cname, "creal");
        int is_imag = !strcmp(cname, "cimagf") || !strcmp(cname, "cimag");
        if (is_real || is_imag) {
            int ok = emit_expr(ch.items[1], out);
            sb_append(out, is_real ? ".re" : ".im");
            free_children(&ch);
            return ok;
        }
    }
    if (!emit_expr(callee, out)) {
        free_children(&ch);
        return 0;
    }
    sb_append(out, "(");
    for (unsigned i = 1; i < ch.count; i++) {
        if (i > 1)
            sb_append(out, ", ");
        if (!emit_expr(ch.items[i], out)) {
            free_children(&ch);
            return 0;
        }
    }
    sb_append(out, ")");
    free_children(&ch);
    return 1;
}

static int emit_member(CXCursor e, sb_t *out) {
    cursors_t ch = get_children(e);
    if (ch.count != 1) {
        free_children(&ch);
        return 0;
    }
    if (!emit_expr(ch.items[0], out)) {
        free_children(&ch);
        return 0;
    }
    free_children(&ch);
    char *name = cxstr(clang_getCursorSpelling(e));
    sb_appendf(out, ".%s", name);
    return 1;
}

static int emit_subscript(CXCursor e, sb_t *out) {
    cursors_t ch = get_children(e);
    if (ch.count != 2) {
        free_children(&ch);
        return 0;
    }
    if (!emit_expr(ch.items[0], out)) {
        free_children(&ch);
        return 0;
    }
    sb_append(out, "[");
    int ok = emit_expr(ch.items[1], out);
    sb_append(out, "]");
    free_children(&ch);
    return ok;
}

static int emit_complex_operand(CXCursor operand, sb_t *out, const char *prefix) {
    CXType oty = clang_getCanonicalType(clang_getCursorType(operand));
    if (oty.kind == CXType_Complex)
        return emit_expr(operand, out);
    sb_appendf(out, "%s_from_real(", prefix);
    int ok = emit_expr(operand, out);
    sb_append(out, ")");
    return ok;
}

static int emit_complex_binary(CXCursor e, sb_t *out, enum CXBinaryOperatorKind k,
                               CXCursor l, CXCursor r, CXType result_ty) {
    const char *prefix = complex_prefix_for(result_ty);
    if (!prefix) {
        note_fail(e, "unsupported complex width");
        return 0;
    }
    mark_complex_used(prefix);
    const char *fn;
    switch (k) {
        case CXBinaryOperator_Add: fn = "add"; break;
        case CXBinaryOperator_Sub: fn = "sub"; break;
        case CXBinaryOperator_Mul: fn = "mul"; break;
        case CXBinaryOperator_Div: fn = "div"; break;
        default:
            note_fail(e, "unsupported complex binary operator");
            return 0;
    }
    sb_appendf(out, "%s_%s(", prefix, fn);
    int ok = emit_complex_operand(l, out, prefix);
    sb_append(out, ", ");
    ok = ok && emit_complex_operand(r, out, prefix);
    sb_append(out, ")");
    return ok;
}

static int emit_binary(CXCursor e, sb_t *out) {
    enum CXBinaryOperatorKind k = clang_getCursorBinaryOperatorKind(e);
    if (k == CXBinaryOperator_Invalid) {
        note_fail(e, "unsupported binary/assignment operator");
        return 0;
    }
    CXType result_ty = clang_getCanonicalType(clang_getCursorType(e));
    if (result_ty.kind == CXType_Complex &&
        (k == CXBinaryOperator_Add || k == CXBinaryOperator_Sub ||
         k == CXBinaryOperator_Mul || k == CXBinaryOperator_Div)) {
        cursors_t ch2 = get_children(e);
        if (ch2.count != 2) {
            note_fail(e, "malformed complex binary operator");
            free_children(&ch2);
            return 0;
        }
        int ok2 = emit_complex_binary(e, out, k, ch2.items[0], ch2.items[1], result_ty);
        free_children(&ch2);
        return ok2;
    }
    char *op = cxstr(clang_getBinaryOperatorKindSpelling(k));
    cursors_t ch = get_children(e);
    if (ch.count != 2) {
        free_children(&ch);
        return 0;
    }
    sb_append(out, "(");
    if (!emit_expr(ch.items[0], out)) {
        free_children(&ch);
        return 0;
    }
    sb_appendf(out, " %s ", op);
    int ok = emit_expr(ch.items[1], out);
    sb_append(out, ")");
    free_children(&ch);
    return ok;
}

static int emit_unary(CXCursor e, sb_t *out) {
    enum CXUnaryOperatorKind k = clang_getCursorUnaryOperatorKind(e);
    cursors_t ch = get_children(e);
    if (ch.count != 1) {
        free_children(&ch);
        return 0;
    }
    CXCursor operand = ch.items[0];
    int ok;
    switch (k) {
        case CXUnaryOperator_PostInc:
            ok = emit_expr(operand, out);
            sb_append(out, "++");
            break;
        case CXUnaryOperator_PostDec:
            ok = emit_expr(operand, out);
            sb_append(out, "--");
            break;
        case CXUnaryOperator_PreInc:
            sb_append(out, "++");
            ok = emit_expr(operand, out);
            break;
        case CXUnaryOperator_PreDec:
            sb_append(out, "--");
            ok = emit_expr(operand, out);
            break;
        case CXUnaryOperator_AddrOf:
            sb_append(out, "&");
            ok = emit_expr(operand, out);
            break;
        case CXUnaryOperator_Deref:
            sb_append(out, "*");
            ok = emit_expr(operand, out);
            break;
        case CXUnaryOperator_Plus:
            ok = emit_expr(operand, out);
            break;
        case CXUnaryOperator_Minus:
            sb_append(out, "-");
            ok = emit_expr(operand, out);
            break;
        case CXUnaryOperator_Not:
            sb_append(out, "~");
            ok = emit_expr(operand, out);
            break;
        case CXUnaryOperator_LNot:
            sb_append(out, "!");
            ok = emit_expr(operand, out);
            break;
        case CXUnaryOperator_Extension:
            ok = emit_expr(operand, out);
            break;
        case CXUnaryOperator_Real:
            ok = emit_expr(operand, out);
            sb_append(out, ".re");
            break;
        case CXUnaryOperator_Imag:
            ok = emit_expr(operand, out);
            sb_append(out, ".im");
            break;
        default:
            note_fail(e, "unsupported unary operator");
            ok = 0;
            break;
    }
    free_children(&ch);
    return ok;
}

static int emit_cast(CXCursor e, sb_t *out) {
    cursors_t ch = get_children(e);
    CXCursor operand = {0};
    for (unsigned i = 0; i < ch.count; i++)
        operand = ch.items[i];
    if (ch.count == 0) {
        free_children(&ch);
        return 0;
    }
    CXType target = clang_getCanonicalType(clang_getCursorType(e));
    if (target.kind == CXType_Complex) {
        const char *prefix = complex_prefix_for(target);
        if (!prefix) {
            note_fail(e, "unsupported complex cast width");
            free_children(&ch);
            return 0;
        }
        mark_complex_used(prefix);
        CXType opty = clang_getCanonicalType(clang_getCursorType(operand));
        int ok;
        if (opty.kind == CXType_Complex) {
            ok = emit_expr(operand, out);
        } else {
            sb_appendf(out, "%s_from_real(", prefix);
            ok = emit_expr(operand, out);
            sb_append(out, ")");
        }
        free_children(&ch);
        return ok;
    }
    char *ty = type_str(clang_getCursorType(e));
    if (!ty) {
        free_children(&ch);
        return 0;
    }
    sb_appendf(out, "cast(%s) (", ty);
    int ok = emit_expr(operand, out);
    sb_append(out, ")");
    free_children(&ch);
    return ok;
}

static int emit_literal_via_eval(CXCursor e, sb_t *out) {
    CXEvalResult ev = clang_Cursor_Evaluate(e);
    if (!ev) {
        note_fail(e, "could not constant-fold expression");
        return 0;
    }
    int ok = 1;
    switch (clang_EvalResult_getKind(ev)) {
        case CXEval_Int:
            sb_appendf(out, "%lld", (long long)clang_EvalResult_getAsLongLong(ev));
            break;
        case CXEval_Float:
            sb_appendf(out, "%g", clang_EvalResult_getAsDouble(ev));
            break;
        case CXEval_StrLiteral: {
            const char *s = clang_EvalResult_getAsStr(ev);
            sb_appendf(out, "\"%s\"", s ? s : "");
            break;
        }
        default:
            note_fail(e, "constant-folded to an unsupported value kind");
            ok = 0;
            break;
    }
    clang_EvalResult_dispose(ev);
    return ok;
}

static int unwrap_designator(CXCursor c, char **field_name_out, CXCursor *value_out) {
    if (clang_getCursorKind(c) != CXCursor_UnexposedExpr)
        return 0;
    cursors_t ch = get_children(c);
    if (ch.count != 2 || clang_getCursorKind(ch.items[0]) != CXCursor_MemberRef) {
        free_children(&ch);
        return 0;
    }
    *field_name_out = cxstr(clang_getCursorSpelling(ch.items[0]));
    *value_out = ch.items[1];
    free_children(&ch);
    return 1;
}

static int emit_init_list(CXCursor e, sb_t *out) {
    CXType t = clang_getCanonicalType(clang_getCursorType(e));
    cursors_t ch = get_children(e);
    if (t.kind == CXType_Record) {
        char *tyname = type_str(clang_getCursorType(e));
        if (!tyname) {
            free_children(&ch);
            return 0;
        }
        CXCursor decl = clang_getTypeDeclaration(t);
        cursors_t fields = get_children(decl);
        sb_appendf(out, "%s { ", tyname);
        unsigned fi = 0;
        int ok = 1;
        for (unsigned i = 0; i < ch.count; i++) {
            char *dname = NULL;
            CXCursor value;
            char *fname;
            if (unwrap_designator(ch.items[i], &dname, &value)) {
                fname = dname;
            } else {
                while (fi < fields.count && clang_getCursorKind(fields.items[fi]) != CXCursor_FieldDecl)
                    fi++;
                if (fi >= fields.count) {
                    ok = 0;
                    break;
                }
                fname = cxstr(clang_getCursorSpelling(fields.items[fi]));
                value = ch.items[i];
                fi++;
            }
            if (i)
                sb_append(out, ", ");
            sb_appendf(out, "%s = ", fname);
            if (!emit_expr(value, out)) {
                ok = 0;
                break;
            }
        }
        sb_append(out, " }");
        free_children(&fields);
        free_children(&ch);
        return ok;
    }
    sb_append(out, "[");
    int ok = 1;
    for (unsigned i = 0; i < ch.count; i++) {
        if (i)
            sb_append(out, ", ");
        if (!emit_expr(ch.items[i], out)) {
            ok = 0;
            break;
        }
    }
    sb_append(out, "]");
    free_children(&ch);
    return ok;
}

static int emit_compound_literal(CXCursor e, sb_t *out) {
    cursors_t ch = get_children(e);
    if (ch.count == 0 || clang_getCursorKind(ch.items[ch.count - 1]) != CXCursor_InitListExpr) {
        note_fail(e, "unsupported compound literal shape");
        free_children(&ch);
        return 0;
    }
    int ok = emit_init_list(ch.items[ch.count - 1], out);
    free_children(&ch);
    return ok;
}

static int emit_imaginary_literal(CXCursor e, sb_t *out) {
    const char *prefix = complex_prefix_for(clang_getCursorType(e));
    if (!prefix) {
        note_fail(e, "unsupported imaginary literal width");
        return 0;
    }
    mark_complex_used(prefix);
    cursors_t ch = get_children(e);
    sb_appendf(out, "%s_from_imag(", prefix);
    int ok = 1;
    if (ch.count == 1)
        ok = emit_expr(ch.items[0], out);
    else
        sb_append(out, "1");
    sb_append(out, ")");
    free_children(&ch);
    return ok;
}

static int emit_stmt_expr(CXCursor e, sb_t *out) {
    int depth = g_cur_depth;
    cursors_t ch = get_children(e);
    CXCursor block = clang_getNullCursor();
    for (unsigned i = 0; i < ch.count; i++)
        if (clang_getCursorKind(ch.items[i]) == CXCursor_CompoundStmt)
            block = ch.items[i];
    free_children(&ch);
    if (clang_Cursor_isNull(block)) {
        note_fail(e, "unsupported statement-expression shape");
        return 0;
    }

    cursors_t body = get_children(block);
    CXType ety = clang_getCanonicalType(clang_getCursorType(e));
    int has_value = ety.kind != CXType_Void && body.count > 0 &&
                    clang_isExpression(clang_getCursorKind(body.items[body.count - 1]));
    unsigned n_plain = has_value ? body.count - 1 : body.count;

    char name[32];
    char *ty = NULL;
    if (has_value) {
        ty = type_str(ety);
        if (!ty) {
            free_children(&body);
            note_fail(e, "unsupported statement-expression result type");
            return 0;
        }
        snprintf(name, sizeof(name), "_stmtexpr%d", g_tmp_n++);
    }

    sb_t accum;
    sb_init(&accum);
    if (has_value) {
        sb_indent(&accum, depth);
        sb_appendf(&accum, "%s: %s;\n", name, ty);
    }

    int ok = 1;
    for (unsigned i = 0; i < n_plain && ok; i++)
        ok = emit_stmt(body.items[i], &accum, depth);

    if (ok && has_value) {
        sb_indent(&accum, depth);
        sb_appendf(&accum, "%s = ", name);
        ok = emit_expr(body.items[body.count - 1], &accum);
        sb_append(&accum, ";\n");
    }

    free_children(&body);
    if (!ok) {
        free(accum.data);
        return 0;
    }

    sb_append(&g_prelude, accum.data);
    free(accum.data);

    if (has_value)
        sb_append(out, name);
    return 1;
}

static int emit_expr(CXCursor e, sb_t *out) {
    e = skip_transparent(e);
    enum CXCursorKind k = clang_getCursorKind(e);
    switch (k) {
        case CXCursor_IntegerLiteral:
        case CXCursor_FloatingLiteral:
        case CXCursor_UnaryExpr:
            return emit_literal_via_eval(e, out);
        case CXCursor_CharacterLiteral: {
            CXEvalResult ev = clang_Cursor_Evaluate(e);
            if (!ev)
                return 0;
            long long v = clang_EvalResult_getAsLongLong(ev);
            clang_EvalResult_dispose(ev);
            if (v >= 32 && v < 127)
                sb_appendf(out, "'%c'", (char)v);
            else
                sb_appendf(out, "cast(char) %lld", v);
            return 1;
        }
        case CXCursor_StringLiteral: {
            char *s = cxstr(clang_getCursorSpelling(e));
            sb_appendf(out, "%s", s);
            return 1;
        }
        case CXCursor_DeclRefExpr: {
            if (emit_enum_const_ref(e, out))
                return 1;
            char *s = cxstr(clang_getCursorSpelling(e));
            sb_append(out, s);
            return 1;
        }
        case CXCursor_CallExpr: return emit_call(e, out);
        case CXCursor_MemberRefExpr: return emit_member(e, out);
        case CXCursor_ArraySubscriptExpr: return emit_subscript(e, out);
        case CXCursor_BinaryOperator:
        case CXCursor_CompoundAssignOperator: return emit_binary(e, out);
        case CXCursor_UnaryOperator: return emit_unary(e, out);
        case CXCursor_CStyleCastExpr: return emit_cast(e, out);
        case CXCursor_InitListExpr: return emit_init_list(e, out);
        case CXCursor_CompoundLiteralExpr: return emit_compound_literal(e, out);
        case CXCursor_ConditionalOperator: return emit_ternary(e, out);
        case CXCursor_ImaginaryLiteral: return emit_imaginary_literal(e, out);
        case CXCursor_StmtExpr: return emit_stmt_expr(e, out);
        default:
            note_fail(e, NULL);
            return 0;
    }
}

static int emit_stmt(CXCursor s, sb_t *out, int depth);
static int emit_stmt_inline_block(CXCursor s, sb_t *out, int depth);

static int emit_body_flat(CXCursor body, sb_t *out, int depth) {
    if (clang_getCursorKind(body) == CXCursor_CompoundStmt) {
        cursors_t ch = get_children(body);
        int ok = 1;
        for (unsigned i = 0; i < ch.count && ok; i++)
            ok = emit_stmt(ch.items[i], out, depth);
        free_children(&ch);
        return ok;
    }
    return emit_stmt(body, out, depth);
}

static int emit_compound(CXCursor s, sb_t *out, int depth) {
    cursors_t ch = get_children(s);
    sb_append(out, "{\n");
    int ok = 1;
    for (unsigned i = 0; i < ch.count && ok; i++)
        ok = emit_stmt(ch.items[i], out, depth + 1);
    sb_indent(out, depth);
    sb_append(out, "}");
    free_children(&ch);
    return ok;
}

static CXCursor find_var_init(CXCursor v) {
    CXType vty = clang_getCanonicalType(clang_getCursorType(v));
    cursors_t vc = get_children(v);
    CXCursor result = clang_getNullCursor();

    if (vty.kind == CXType_ConstantArray && vc.count >= 1) {
        long long declared = clang_getArraySize(vty);
        CXEvalResult ev = clang_Cursor_Evaluate(vc.items[0]);
        int first_is_bound = 0;
        if (ev) {
            if (clang_EvalResult_getKind(ev) == CXEval_Int &&
                clang_EvalResult_getAsLongLong(ev) == declared)
                first_is_bound = 1;
            clang_EvalResult_dispose(ev);
        }
        unsigned last = vc.count - 1;
        if (first_is_bound) {
            if (vc.count >= 2 && clang_isExpression(clang_getCursorKind(vc.items[last])))
                result = vc.items[last];
        } else if (clang_isExpression(clang_getCursorKind(vc.items[last]))) {
            result = vc.items[last];
        }
    } else if (vc.count > 0 && clang_isExpression(clang_getCursorKind(vc.items[vc.count - 1]))) {
        result = vc.items[vc.count - 1];
    }

    free_children(&vc);
    return result;
}

static int emit_decl_stmt(CXCursor s, sb_t *out, int depth) {
    cursors_t ch = get_children(s);
    int ok = 1;
    for (unsigned i = 0; i < ch.count && ok; i++) {
        CXCursor v = ch.items[i];
        if (clang_getCursorKind(v) != CXCursor_VarDecl) {
            note_fail(v, "non-variable declaration inside a function body (e.g. a local struct/typedef)");
            ok = 0;
            break;
        }
        char *name = cxstr(clang_getCursorSpelling(v));
        char *ty = type_str(clang_getCursorType(v));
        if (!ty) {
            ok = 0;
            break;
        }
        CXCursor init = find_var_init(v);
        sb_t line;
        sb_init(&line);
        sb_indent(&line, depth);
        sb_appendf(&line, "%s: %s", name, ty);
        prelude_reset();
        if (!clang_Cursor_isNull(init)) {
            sb_append(&line, " = ");
            ok = emit_expr(init, &line);
        }
        sb_append(&line, ";\n");
        if (ok) {
            sb_append(out, g_prelude.data);
            sb_append(out, line.data);
        }
        free(line.data);
    }
    free_children(&ch);
    return ok;
}

static int emit_if(CXCursor s, sb_t *out, int depth) {
    cursors_t ch = get_children(s);
    if (ch.count < 2 || ch.count > 3) {
        note_fail(s, "unexpected 'if' statement shape");
        free_children(&ch);
        return 0;
    }
    sb_t cond;
    sb_init(&cond);
    prelude_reset();
    int ok = emit_expr(ch.items[0], &cond);
    if (!ok) {
        free(cond.data);
        free_children(&ch);
        return 0;
    }
    sb_append(out, g_prelude.data);
    sb_indent(out, depth);
    sb_append(out, "if ");
    sb_append(out, cond.data);
    free(cond.data);
    sb_append(out, " ");
    ok = emit_stmt_inline_block(ch.items[1], out, depth);
    if (ok && ch.count == 3) {
        sb_append(out, " else ");
        ok = emit_stmt_inline_block(ch.items[2], out, depth);
    }
    sb_append(out, "\n");
    free_children(&ch);
    return ok;
}

static int emit_while(CXCursor s, sb_t *out, int depth) {
    cursors_t ch = get_children(s);
    if (ch.count != 2) {
        note_fail(s, "unexpected 'while' statement shape");
        free_children(&ch);
        return 0;
    }
    sb_t cond;
    sb_init(&cond);
    prelude_reset();
    int ok = emit_expr(ch.items[0], &cond);
    if (ok) {
        sb_append(out, g_prelude.data);
        sb_indent(out, depth);
        sb_append(out, "while ");
        sb_append(out, cond.data);
        sb_append(out, " ");
        ok = emit_stmt_inline_block(ch.items[1], out, depth);
    }
    free(cond.data);
    sb_append(out, "\n");
    free_children(&ch);
    return ok;
}

static int emit_do(CXCursor s, sb_t *out, int depth) {
    cursors_t ch = get_children(s);
    if (ch.count != 2) {
        free_children(&ch);
        return 0;
    }
    sb_indent(out, depth);
    sb_append(out, "while true {\n");
    int ok = emit_body_flat(ch.items[0], out, depth + 1);
    sb_indent(out, depth + 1);
    sb_append(out, "if !(");
    if (ok)
        ok = emit_expr(ch.items[1], out);
    sb_append(out, ") then break;\n");
    sb_indent(out, depth);
    sb_append(out, "}\n");
    free_children(&ch);
    return ok;
}

static int emit_for(CXCursor s, sb_t *out, int depth) {
    cursors_t ch = get_children(s);
    if (ch.count != 4) {
        note_fail(s, "'for' loop missing an init/condition/increment clause");
        free_children(&ch);
        return 0;
    }
    int ok = 1;
    if (clang_getCursorKind(ch.items[0]) == CXCursor_DeclStmt)
        ok = emit_decl_stmt(ch.items[0], out, depth);
    else {
        sb_t line;
        sb_init(&line);
        sb_indent(&line, depth);
        prelude_reset();
        ok = emit_expr(ch.items[0], &line);
        sb_append(&line, ";\n");
        if (ok) {
            sb_append(out, g_prelude.data);
            sb_append(out, line.data);
        }
        free(line.data);
    }
    if (!ok) {
        free_children(&ch);
        return 0;
    }

    sb_t cond;
    sb_init(&cond);
    prelude_reset();
    ok = emit_expr(ch.items[1], &cond);
    if (!ok) {
        free(cond.data);
        free_children(&ch);
        return 0;
    }
    sb_append(out, g_prelude.data);
    sb_indent(out, depth);
    sb_append(out, "while ");
    sb_append(out, cond.data);
    free(cond.data);
    sb_append(out, " {\n");

    ok = emit_body_flat(ch.items[3], out, depth + 1);

    sb_t inc;
    sb_init(&inc);
    prelude_reset();
    ok = ok && emit_expr(ch.items[2], &inc);
    if (ok) {
        sb_append(out, g_prelude.data);
        sb_indent(out, depth + 1);
        sb_append(out, inc.data);
        sb_append(out, ";\n");
    }
    free(inc.data);
    sb_indent(out, depth);
    sb_append(out, "}\n");
    free_children(&ch);
    return ok;
}

static int emit_stmt_inline_block(CXCursor s, sb_t *out, int depth) {
    if (clang_getCursorKind(s) == CXCursor_CompoundStmt)
        return emit_compound(s, out, depth);
    sb_append(out, "{\n");
    int ok = emit_stmt(s, out, depth + 1);
    sb_indent(out, depth);
    sb_append(out, "}");
    return ok;
}

static int emit_stmt(CXCursor s, sb_t *out, int depth) {
    g_cur_depth = depth;
    enum CXCursorKind k = clang_getCursorKind(s);
    switch (k) {
        case CXCursor_CompoundStmt:
            sb_indent(out, depth);
            return emit_compound(s, out, depth) && (sb_append(out, "\n"), 1);
        case CXCursor_DeclStmt:
            return emit_decl_stmt(s, out, depth);
        case CXCursor_IfStmt:
            return emit_if(s, out, depth);
        case CXCursor_WhileStmt:
            return emit_while(s, out, depth);
        case CXCursor_DoStmt:
            return emit_do(s, out, depth);
        case CXCursor_ForStmt:
            return emit_for(s, out, depth);
        case CXCursor_ReturnStmt: {
            cursors_t ch = get_children(s);
            sb_t line;
            sb_init(&line);
            sb_indent(&line, depth);
            sb_append(&line, "return");
            prelude_reset();
            int ok = 1;
            if (ch.count == 1) {
                sb_append(&line, " ");
                ok = emit_expr(ch.items[0], &line);
            }
            sb_append(&line, ";\n");
            if (ok) {
                sb_append(out, g_prelude.data);
                sb_append(out, line.data);
            }
            free(line.data);
            free_children(&ch);
            return ok;
        }
        case CXCursor_BreakStmt:
            sb_indent(out, depth);
            sb_append(out, "break;\n");
            return 1;
        case CXCursor_ContinueStmt:
            sb_indent(out, depth);
            sb_append(out, "continue;\n");
            return 1;
        case CXCursor_NullStmt:
            return 1;
        case CXCursor_LabelStmt: {
            char *name = cxstr(clang_getCursorSpelling(s));
            sb_indent(out, depth);
            sb_appendf(out, "label %s;\n", name);
            cursors_t ch = get_children(s);
            int ok = 1;
            if (ch.count == 1)
                ok = emit_stmt(ch.items[0], out, depth);
            free_children(&ch);
            return ok;
        }
        case CXCursor_GotoStmt: {
            char *name = cxstr(clang_getCursorSpelling(s));
            sb_indent(out, depth);
            sb_appendf(out, "goto %s;\n", name);
            return 1;
        }
        default: {
            sb_t line;
            sb_init(&line);
            sb_indent(&line, depth);
            prelude_reset();
            int ok = emit_expr(s, &line);
            sb_append(&line, ";\n");
            if (ok) {
                sb_append(out, g_prelude.data);
                sb_append(out, line.data);
            }
            free(line.data);
            return ok;
        }
    }
}

/* Header doc comments -> `//` lines. */
static void emit_doc(CXCursor d, sb_t *out, int depth) {
#if STBIND_EMIT_DOCS
    CXString raw = clang_Cursor_getRawCommentText(d);
    const char *c = clang_getCString(raw);
    if (c && *c) {
        const char *p = c;
        for (;;) {
            const char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            char *line = stbind_arena_push(len + 1);
            memcpy(line, p, len);
            line[len] = 0;

            char *t = line;
            while (*t == ' ' || *t == '\t' || *t == '\r')
                t++;
            if (!strncmp(t, "/**", 3) || !strncmp(t, "/*!", 3) || !strncmp(t, "///", 3) ||
                !strncmp(t, "//!", 3))
                t += 3;
            else if (!strncmp(t, "/*", 2) || !strncmp(t, "//", 2))
                t += 2;
            else if (*t == '*' && t[1] != '/')
                t += 1;
            size_t l = strlen(t);
            if (l >= 2 && t[l - 2] == '*' && t[l - 1] == '/')
                t[l - 2] = 0;
            l = strlen(t);
            while (l && (t[l - 1] == ' ' || t[l - 1] == '\t' || t[l - 1] == '\r'))
                t[--l] = 0;
            while (*t == ' ' || *t == '\t')
                t++;
            if (*t) {
                sb_indent(out, depth);
                sb_appendf(out, "// %s\n", t);
            }
            if (!e)
                break;
            p = e + 1;
        }
    }
    clang_disposeString(raw);
#else
    (void)d; (void)out; (void)depth;
#endif
}

static void emit_extern_fn(CXCursor f, sb_t *out) {
    char *name = cxstr(clang_getCursorSpelling(f));
    fail_reset();
    char *ret = type_str(clang_getResultType(clang_getCursorType(f)));
    if (!ret) {
        fprintf(stderr, "stbind: skipping function '%s' (%s)\n", name,
                g_fail_set ? g_fail.data : "unsupported return type");
        return;
    }
    sb_t decl;
    sb_init(&decl);
    sb_appendf(&decl, "extern fn %s(", name);
    int n = clang_Cursor_getNumArguments(f);
    for (int i = 0; i < n; i++) {
        if (i)
            sb_append(&decl, ", ");
        CXCursor arg = clang_Cursor_getArgument(f, i);
        char *aname = cxstr(clang_getCursorSpelling(arg));
        fail_reset();
        char *aty = param_type_str(clang_getCursorType(arg));
        if (!aty) {
            fprintf(stderr, "stbind: skipping function '%s' (%s)\n", name,
                    g_fail_set ? g_fail.data : "unsupported parameter type");
            free(decl.data);
            return;
        }
        if (!aname[0])
            sb_appendf(&decl, "a%d: %s", i, aty);
        else
            sb_appendf(&decl, "%s: %s", safe_ident(aname), aty);
    }
    if (clang_isFunctionTypeVariadic(clang_getCursorType(f)))
        sb_append(&decl, n ? ", ..." : "...");
    sb_appendf(&decl, ") -> %s;\n", ret);
    emit_doc(f, out, 0);
    sb_append(out, decl.data);
    free(decl.data);
}

static void emit_extern_var(CXCursor v, sb_t *out) {
#if STBIND_EMIT_EXTERN_VARS
    if (clang_Cursor_getStorageClass(v) != CX_SC_Extern || clang_isCursorDefinition(v))
        return;
    char *name = cxstr(clang_getCursorSpelling(v));
    if (!name[0] || is_reserved(name))
        return;
    CXType t = clang_getCursorType(v);
    if (clang_getCanonicalType(t).kind == CXType_IncompleteArray) {
        fprintf(stderr, "stbind: skipping extern variable '%s' (array of unknown size)\n", name);
        return;
    }
    fail_reset();
    char *ty = type_str(t);
    if (!ty) {
        fprintf(stderr, "stbind: skipping extern variable '%s' (%s)\n", name,
                g_fail_set ? g_fail.data : "unsupported type");
        return;
    }
    emit_doc(v, out, 0);
    sb_appendf(out, "extern %s: %s;\n\n", name, ty);
#else
    (void)v; (void)out;
#endif
}

static int try_emit_full_fn(CXCursor f, sb_t *out) {
    sb_t body;
    sb_init(&body);

    char *name = cxstr(clang_getCursorSpelling(f));
    char *ret = type_str(clang_getResultType(clang_getCursorType(f)));
    if (!ret) {
        free(body.data);
        return 0;
    }

    sb_appendf(&body, "fn %s(", name);
    int n = clang_Cursor_getNumArguments(f);
    for (int i = 0; i < n; i++) {
        if (i)
            sb_append(&body, ", ");
        CXCursor arg = clang_Cursor_getArgument(f, i);
        char *aname = cxstr(clang_getCursorSpelling(arg));
        char *aty = param_type_str(clang_getCursorType(arg));
        if (!aty) {
            free(body.data);
            return 0;
        }
        if (!aname[0])
            sb_appendf(&body, "a%d: %s", i, aty);
        else
            sb_appendf(&body, "%s: %s", safe_ident(aname), aty);
    }
    sb_appendf(&body, ") -> %s ", ret);

    CXCursor block = clang_getNullCursor();
    {
        cursors_t ch = get_children(f);
        for (unsigned i = 0; i < ch.count; i++)
            if (clang_getCursorKind(ch.items[i]) == CXCursor_CompoundStmt)
                block = ch.items[i];
        free_children(&ch);
    }
    if (clang_Cursor_isNull(block)) {
        free(body.data);
        return 0;
    }

    if (!emit_compound(block, &body, 0)) {
        free(body.data);
        return 0;
    }
    sb_append(&body, "\n");
    sb_append(out, body.data);
    free(body.data);
    return 1;
}

static const char *record_directive(CXCursor d) {
    if (clang_getCursorKind(d) == CXCursor_UnionDecl)
        return STBIND_UNION_DIRECTIVE;
    cursors_t ch = get_children(d);
    int packed = 0;
    for (unsigned i = 0; i < ch.count; i++)
        if (clang_getCursorKind(ch.items[i]) == CXCursor_PackedAttr)
            packed = 1;
    free_children(&ch);
    return packed ? STBIND_PACK_DIRECTIVE : "";
}

/* Tracks where our emitted layout ends so we can match C offsets with explicit padding. */
typedef struct {
    sb_t *body;
    int depth, is_union;
    long long cur;
    int pad_n, bits_n, anon_n;
    int in_bits;
    long long bit_first, bit_last;
    sb_t bit_names;
    int have_pending;
    CXCursor pending;
} fields_ctx_t;

static int emit_fields(CXCursor d, sb_t *body, int depth);

static long long type_size_or_0(CXType t) {
    long long sz = clang_Type_getSizeOf(t);
    return sz > 0 ? sz : 0;
}

static void fields_pad(fields_ctx_t *f, long long n) {
    if (n <= 0)
        return;
    sb_indent(f->body, f->depth);
    sb_appendf(f->body, "_pad%d: [%lld]u8;\n", f->pad_n++, n);
    f->cur += n;
}

/* Called right before emitting a field of type `fty` that C puts at `off_bits`. */
static void fields_place(fields_ctx_t *f, CXType fty, long long off_bits) {
    if (f->is_union)
        return;
    long long al = clang_Type_getAlignOf(fty);
    if (al < 1)
        al = 1;
    long long ours = (f->cur + al - 1) / al * al;
    if (off_bits >= 0) {
        long long off = off_bits / 8;
        if (ours < off)
            fields_pad(f, off - f->cur);
        f->cur = off + type_size_or_0(fty);
    } else {
        f->cur = ours + type_size_or_0(fty);
    }
}

/* A run of bitfields cannot be expressed; keep its exact bytes as one opaque field. */
static void fields_flush_bits(fields_ctx_t *f) {
    if (!f->in_bits)
        return;
    long long end = (f->bit_last + 7) / 8;
    long long start = f->bit_first / 8;
    if (f->is_union) {
        start = 0;
    } else if (start > f->cur) {
        fields_pad(f, start - f->cur);
    } else {
        start = f->cur;
    }
    long long len = end - start;
    if (len > 0) {
        sb_indent(f->body, f->depth);
        sb_appendf(f->body, "_bits%d: [%lld]u8;", f->bits_n++, len);
        if (f->bit_names.len)
            sb_appendf(f->body, " // bitfields: %s", f->bit_names.data);
        sb_append(f->body, "\n");
        if (!f->is_union)
            f->cur = end;
    }
    f->in_bits = 0;
    f->bit_names.len = 0;
    if (f->bit_names.data)
        f->bit_names.data[0] = 0;
}

static int fields_emit_anon(fields_ctx_t *f, CXCursor rec, const char *fname) {
    sb_t inner;
    sb_init(&inner);
    if (!emit_fields(rec, &inner, f->depth + 1)) {
        free(inner.data);
        return 0;
    }
    sb_indent(f->body, f->depth);
    sb_appendf(f->body, "%s: struct%s {\n", fname, record_directive(rec));
    sb_append(f->body, inner.data);
    sb_indent(f->body, f->depth);
    sb_append(f->body, "}\n");
    free(inner.data);
    return 1;
}

/* An anonymous struct/union member with no field cursor of its own (C11). */
static void fields_flush_pending(fields_ctx_t *f) {
    if (!f->have_pending)
        return;
    f->have_pending = 0;
    char nm[32];
    snprintf(nm, sizeof(nm), "_anon%d", f->anon_n++);
    CXType t = clang_getCursorType(f->pending);
    fields_flush_bits(f);
    fields_place(f, t, -1);
    if (!fields_emit_anon(f, f->pending, nm)) {
        long long sz = type_size_or_0(t);
        if (sz > 0) {
            sb_indent(f->body, f->depth);
            sb_appendf(f->body, "%s: [%lld]u8;\n", nm, sz);
        }
    }
}

static int emit_fields(CXCursor d, sb_t *body, int depth) {
    fields_ctx_t f;
    memset(&f, 0, sizeof(f));
    f.body = body;
    f.depth = depth;
    f.is_union = clang_getCursorKind(d) == CXCursor_UnionDecl;
    sb_init(&f.bit_names);

    int ok = 1;
    cursors_t ch = get_children(d);
    for (unsigned i = 0; ok && i < ch.count; i++) {
        CXCursor c = ch.items[i];
        enum CXCursorKind k = clang_getCursorKind(c);

        if (k == CXCursor_StructDecl || k == CXCursor_UnionDecl) {
            if (!clang_isCursorDefinition(c))
                continue;
            if (decl_is_anonymous(c)) {
                fields_flush_pending(&f);
                f.pending = c;
                f.have_pending = 1;
            } else {
                char *nm = cxstr(clang_getCursorSpelling(c));
                if (nm[0])
                    hset_add(&g_record_index, nm, c);
            }
            continue;
        }
        if (k == CXCursor_EnumDecl) {
            if (!decl_is_anonymous(c)) {
                char *nm = cxstr(clang_getCursorSpelling(c));
                if (nm[0])
                    hset_add(&g_record_index, nm, c);
            }
            continue;
        }
        if (k != CXCursor_FieldDecl)
            continue;

        char *fname = cxstr(clang_getCursorSpelling(c));
        CXType fty = clang_getCursorType(c);
        CXType fcanon = clang_getCanonicalType(fty);
        long long off_bits = clang_Cursor_getOffsetOfField(c);

        if (clang_Cursor_isBitField(c)) {
            long long w = clang_getFieldDeclBitWidth(c);
            if (w > 0) {
                fields_flush_pending(&f);
                long long start = off_bits >= 0 ? off_bits : (f.in_bits ? f.bit_last : f.cur * 8);
                if (!f.in_bits) {
                    f.in_bits = 1;
                    f.bit_first = start;
                    f.bit_last = start;
                }
                if (start + w > f.bit_last)
                    f.bit_last = start + w;
                if (fname[0]) {
                    if (f.bit_names.len)
                        sb_append(&f.bit_names, ", ");
                    sb_appendf(&f.bit_names, "%s:%lld", fname, w);
                }
            }
            continue;
        }
        fields_flush_bits(&f);

        CXCursor tdecl = clang_getNullCursor();
        int anon_rec = 0;
        {
            /* only a literally anonymous `struct { } x;` is inlined; a typedef name is kept */
            CXType declared = fty;
            if (declared.kind == CXType_Elaborated)
                declared = clang_Type_getNamedType(declared);
            if (declared.kind == CXType_Record && fcanon.kind == CXType_Record) {
                tdecl = clang_getTypeDeclaration(fcanon);
                anon_rec = decl_is_anonymous(tdecl);
            }
        }
        if (f.have_pending) {
            if (anon_rec && clang_equalCursors(f.pending, tdecl))
                f.have_pending = 0;
            else
                fields_flush_pending(&f);
        }

        char nm[32];
        const char *emit_name = fname;
        if (!fname[0]) {
            snprintf(nm, sizeof(nm), "_anon%d", f.anon_n++);
            emit_name = nm;
        }
        emit_name = safe_ident(emit_name);

        if (fcanon.kind == CXType_IncompleteArray) {
            sb_indent(body, depth);
            sb_appendf(body, "// flexible array member '%s' omitted\n", fname);
            continue;
        }

        if (anon_rec) {
            fields_place(&f, fty, off_bits);
            if (fields_emit_anon(&f, tdecl, emit_name))
                continue;
            long long sz = type_size_or_0(fcanon);
            if (sz > 0) {
                sb_indent(body, depth);
                sb_appendf(body, "%s: [%lld]u8;\n", emit_name, sz);
                continue;
            }
            ok = 0;
            break;
        }

        fail_reset();
        char *fty_str = type_str(fty);
        fields_place(&f, fty, off_bits);
        if (!fty_str) {
            /* unsupported type: keep the field's bytes so the layout stays right */
            long long sz = type_size_or_0(fcanon);
            if (sz > 0) {
                sb_indent(body, depth);
                sb_appendf(body, "%s: [%lld]u8; // opaque: %s\n", emit_name, sz,
                           g_fail_set ? g_fail.data : "unsupported type");
                continue;
            }
            ok = 0;
            break;
        }
        sb_indent(body, depth);
        sb_appendf(body, "%s: %s;\n", emit_name, fty_str);
    }
    if (ok) {
        fields_flush_bits(&f);
        fields_flush_pending(&f);
        if (!f.is_union) {
            CXType rt = clang_getCursorType(d);
            long long csz = clang_Type_getSizeOf(rt);
            long long cal = clang_Type_getAlignOf(rt);
            int packed = !strcmp(record_directive(d), STBIND_PACK_DIRECTIVE);
            if (cal < 1 || packed)
                cal = 1;
            long long ours = (f.cur + cal - 1) / cal * cal;
            if (csz > ours)
                fields_pad(&f, csz - f.cur);
        }
    }
    free(f.bit_names.data);
    free_children(&ch);
    return ok;
}

static void emit_enum(CXCursor d, const char *name, sb_t *out) {
    CXType it = clang_getEnumDeclIntegerType(d);
    CXType icanon = clang_getCanonicalType(it);
    int is_unsigned = icanon.kind == CXType_UChar || icanon.kind == CXType_UShort ||
                      icanon.kind == CXType_UInt || icanon.kind == CXType_ULong ||
                      icanon.kind == CXType_ULongLong;

    const char *backing = NULL;
    {
        sb_t ty;
        sb_init(&ty);
        fail_reset();
        if (map_type(it, &ty) && strcmp(ty.data, "u32") != 0)
            backing = stbind_arena_strdup(ty.data);
        free(ty.data);
    }

    cursors_t ch = get_children(d);
    unsigned n = 0;
    long long *vals = NULL;
    char **names = NULL;
    vals = stbind_arena_push(sizeof(*vals) * (ch.count + 1));
    names = stbind_arena_push(sizeof(*names) * (ch.count + 1));
    for (unsigned i = 0; i < ch.count; i++) {
        if (clang_getCursorKind(ch.items[i]) != CXCursor_EnumConstantDecl)
            continue;
        names[n] = cxstr(clang_getCursorSpelling(ch.items[i]));
        vals[n] = is_unsigned ? (long long)clang_getEnumConstantDeclUnsignedValue(ch.items[i])
                              : clang_getEnumConstantDeclValue(ch.items[i]);
        n++;
    }

    /* enum_flag: variant i is (first << i), only the first may carry an explicit value */
    int as_flag = 0;
#if STBIND_DETECT_ENUM_FLAGS
    if (n >= 3 && n <= 30 && vals[0] > 0 && vals[0] < (1LL << 30)) {
        as_flag = 1;
        for (unsigned i = 0; i < n; i++)
            if (vals[i] != (vals[0] << i)) {
                as_flag = 0;
                break;
            }
    }
#endif

    int strip = enum_strip_len(d);
    emit_doc(d, out, 0);
    sb_appendf(out, "%s %s", as_flag ? "enum_flag" : "enum", name);
    if (backing)
        sb_appendf(out, " : %s", backing);
    sb_append(out, " {\n");

    for (unsigned i = 0; i < n; i++) {
        const_add(names[i], vals[i]);
        const char *vn = safe_ident(names[i] + strip);

        /* storth rejects two variants with the same value; keep aliases as comments */
        unsigned dup = n;
        for (unsigned j = 0; j < i; j++)
            if (vals[j] == vals[i]) {
                dup = j;
                break;
            }
        if (dup != n) {
            sb_appendf(out, "    // %s == %s\n", vn, safe_ident(names[dup] + strip));
            continue;
        }
        if (as_flag) {
            if (i == 0)
                sb_appendf(out, "    %s := %lld,\n", vn, vals[i]);
            else
                sb_appendf(out, "    %s,\n", vn);
        } else if (is_unsigned) {
            sb_appendf(out, "    %s := %llu,\n", vn, (unsigned long long)vals[i]);
        } else {
            sb_appendf(out, "    %s := %lld,\n", vn, vals[i]);
        }
    }
    sb_append(out, "}\n\n");
    free_children(&ch);
}

static void emit_record_as(CXCursor d, const char *forced_name, sb_t *out) {
    if (!forced_name && decl_is_anonymous(d))
        return;
    char *name = forced_name ? stbind_arena_strdup(forced_name)
                             : cxstr(clang_getCursorSpelling(d));
    if (!name[0]) {
        return;
    }

    if (hset_has(&g_emitted_types, name)) {
        return;
    }

    enum CXCursorKind k = clang_getCursorKind(d);
    if (k == CXCursor_EnumDecl) {
        hset_add(&g_emitted_types, name, clang_getNullCursor());
        emit_enum(d, name, out);
        return;
    }
    if (k != CXCursor_StructDecl && k != CXCursor_UnionDecl) {
        return;
    }
    if (!clang_isCursorDefinition(d)) {
        return;
    }
    hset_add(&g_emitted_types, name, clang_getNullCursor());
    sb_t body;
    sb_init(&body);
    fail_reset();
    int ok = emit_fields(d, &body, 1);
    if (ok) {
        emit_doc(d, out, 0);
        sb_appendf(out, "struct %s%s {\n%s}\n\n", name, record_directive(d), body.data);
    } else {
        fprintf(stderr, "stbind: skipping %s '%s' (unsupported field type)\n",
                k == CXCursor_UnionDecl ? "union" : "struct", name);
    }
    free(body.data);
}

static void emit_record(CXCursor d, sb_t *out) {
    emit_record_as(d, NULL, out);
}

/* libclang spells anonymous tags differently across versions, so check both. */
static int decl_is_anonymous(CXCursor d) {
    if (clang_Cursor_isAnonymous(d))
        return 1;
    char *n = cxstr(clang_getCursorSpelling(d));
    return !n[0] || strstr(n, "unnamed") || strstr(n, "anonymous");
}

/* True for scalar C types that map_type already writes inline (i32, f64, bool...). */
static int is_scalar_kind(enum CXTypeKind k) {
    switch (k) {
        case CXType_Bool:
        case CXType_Char_S: case CXType_Char_U:
        case CXType_SChar:  case CXType_UChar:
        case CXType_Short:  case CXType_UShort:
        case CXType_Int:    case CXType_UInt:
        case CXType_Long:   case CXType_ULong:
        case CXType_LongLong: case CXType_ULongLong:
        case CXType_Float:  case CXType_Double: case CXType_LongDouble:
        case CXType_Complex:
            return 1;
        default:
            return 0;
    }
}

/*
 * A typedef is "primitive" when it only renames a scalar, or a pointer/array of
 * scalars (typedef unsigned int uint; typedef const char *cstr;).  map_type
 * already collapses these to their canonical spelling at every use site, so an
 * alias would be dead weight.  void* handles, function pointers, structs,
 * enums and opaque types are NOT primitive: those keep their alias.
 */
static int typedef_is_primitive(CXType underlying) {
    CXType t = clang_getCanonicalType(underlying);
    for (;;) {
        if (t.kind == CXType_Pointer) {
            CXType pointee = clang_getCanonicalType(clang_getPointeeType(t));
            if (pointee.kind == CXType_FunctionProto || pointee.kind == CXType_FunctionNoProto)
                return 0;
            if (pointee.kind == CXType_Void)
                return 0;
            t = pointee;
        } else if (t.kind == CXType_ConstantArray || t.kind == CXType_IncompleteArray) {
            t = clang_getCanonicalType(clang_getArrayElementType(t));
        } else {
            break;
        }
    }
    return is_scalar_kind(t.kind);
}

static void emit_typedef(CXCursor d, sb_t *out) {
    char *name = cxstr(clang_getCursorSpelling(d));
    if (!name[0])
        return;

    if (!strcmp(name, "__va_list_tag")) {
        if (!hset_has(&g_emitted_types, name)) {
            hset_add(&g_emitted_types, name, clang_getNullCursor());
            sb_append(out, "using __va_list_tag *void;\n\n");
        }
        return;
    }

    CXType underlying = clang_getTypedefDeclUnderlyingType(d);
    CXType canon = clang_getCanonicalType(underlying);

    /* Primitive typedefs never get an alias; uses are already inlined. */
    if (typedef_is_primitive(underlying))
        return;

    /* typedef enum { A, B } Name;  ->  enum Name { ... } */
    if (canon.kind == CXType_Enum) {
        CXCursor enum_decl = clang_getTypeDeclaration(canon);
        if (decl_is_anonymous(enum_decl)) {
            emit_record_as(enum_decl, name, out);
            return;
        }
    }

    if (canon.kind == CXType_Record) {
        CXCursor record_decl = clang_getTypeDeclaration(canon);
        char *record_name = cxstr(clang_getCursorSpelling(record_decl));

        if (record_name[0] && !strcmp(record_name, "__va_list_tag")) {
            if (!hset_has(&g_emitted_types, name)) {
                hset_add(&g_emitted_types, name, clang_getNullCursor());
                sb_appendf(out, "using %s *void;\n\n", name);
            }
            return;
        }

        int same_name = !strcmp(record_name, name);

        /* typedef struct { ... } Name;  ->  struct Name { ... } */
        if (decl_is_anonymous(record_decl)) {
            if (clang_isCursorDefinition(record_decl))
                emit_record_as(record_decl, name, out);
            return;
        }

        if (same_name)
            return;

        if (!clang_isCursorDefinition(record_decl)) {
            if (!hset_has(&g_emitted_types, name)) {
                hset_add(&g_emitted_types, name, clang_getNullCursor());
                sb_appendf(out, "using %s *void;\n\n", name);
            }
            return;
        }

        char *target = type_str(canon);
        if (target && strcmp(name, target) != 0 &&
            !hset_has(&g_emitted_types, name)) {
            hset_add(&g_emitted_types, name, clang_getNullCursor());
            sb_appendf(out, "using %s %s;\n\n", name, target);
        }
        return;
    }

    fail_reset();
    char *target = type_str(underlying);
    if (!target) {
        fprintf(stderr, "stbind: skipping typedef '%s' (%s)\n",
                name,
                g_fail_set ? g_fail.data : "unsupported underlying type");
        return;
    }

    if (strcmp(name, target) != 0 &&
        !hset_has(&g_emitted_types, name)) {
        hset_add(&g_emitted_types, name, clang_getNullCursor());
        sb_appendf(out, "using %s %s;\n\n", name, target);
    }
}


/* #define NAME <constant>  ->  NAME :: <value>; */

typedef struct {
    const char **t;
    unsigned n, pos;
    int err;
} mx_t;

static int mx_is(mx_t *m, const char *s) {
    return m->pos < m->n && !strcmp(m->t[m->pos], s);
}

static long long mx_int_literal(const char *s, int *ok) {
    char buf[80];
    size_t len = strlen(s);
    *ok = 0;
    if (!len || len >= sizeof(buf))
        return 0;
    memcpy(buf, s, len + 1);
    while (len && strchr("uUlL", buf[len - 1]))
        buf[--len] = 0;
    if (!len)
        return 0;
    char *end;
    unsigned long long v;
    if (len > 2 && buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B'))
        v = strtoull(buf + 2, &end, 2);
    else
        v = strtoull(buf, &end, 0);
    *ok = *end == 0;
    return (long long)v;
}

static long long mx_char_literal(const char *s, int *ok) {
    size_t len = strlen(s);
    *ok = 0;
    if (len < 3 || s[0] != '\'' || s[len - 1] != '\'')
        return 0;
    if (s[1] != '\\') {
        *ok = len == 3;
        return (unsigned char)s[1];
    }
    *ok = 1;
    switch (s[2]) {
        case 'n': return 10;
        case 't': return 9;
        case 'r': return 13;
        case 'a': return 7;
        case 'b': return 8;
        case 'f': return 12;
        case 'v': return 11;
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        case 'x': return strtol(s + 3, NULL, 16);
        default:
            if (s[2] >= '0' && s[2] <= '7')
                return strtol(s + 2, NULL, 8);
    }
    *ok = 0;
    return 0;
}

static int mx_prec(const char *op) {
    if (!strcmp(op, "|")) return 1;
    if (!strcmp(op, "^")) return 2;
    if (!strcmp(op, "&")) return 3;
    if (!strcmp(op, "<<") || !strcmp(op, ">>")) return 4;
    if (!strcmp(op, "+") || !strcmp(op, "-")) return 5;
    if (!strcmp(op, "*") || !strcmp(op, "/") || !strcmp(op, "%")) return 6;
    return 0;
}

static long long mx_binary(mx_t *m, int minprec);

static long long mx_unary(mx_t *m) {
    if (m->pos >= m->n) {
        m->err = 1;
        return 0;
    }
    const char *t = m->t[m->pos];
    if (!strcmp(t, "-")) {
        m->pos++;
        return (long long)(0ULL - (unsigned long long)mx_unary(m));
    }
    if (!strcmp(t, "+")) {
        m->pos++;
        return mx_unary(m);
    }
    if (!strcmp(t, "~")) {
        m->pos++;
        return ~mx_unary(m);
    }
    if (!strcmp(t, "!")) {
        m->pos++;
        return !mx_unary(m);
    }
    if (!strcmp(t, "(")) {
        /* "(in_addr_t) 0x7f000001", "(unsigned long) 5": a cast we can ignore for the value */
        {
            static const char *const kw[] = {"unsigned", "signed", "int", "long", "short",
                                             "char", "float", "double", "void", "const", NULL};
            unsigned q = m->pos + 1;
            int words = 0, typeish = 0, bad = 0;
            while (q < m->n && strcmp(m->t[q], ")")) {
                const char *w = m->t[q];
                long long tmp;
                if (!strcmp(w, "*")) {
                    typeish = 1;
                } else if ((isalpha((unsigned char)w[0]) || w[0] == '_') && !const_get(w, &tmp)) {
                    size_t wl = strlen(w);
                    for (int i = 0; kw[i]; i++)
                        if (!strcmp(w, kw[i]))
                            typeish = 1;
                    if (wl > 2 && !strcmp(w + wl - 2, "_t"))
                        typeish = 1;
                    words++;
                } else {
                    bad = 1;
                    break;
                }
                q++;
            }
            if (!bad && words && typeish && q + 1 < m->n && !strcmp(m->t[q], ")")) {
                m->pos = q + 1;
                return mx_unary(m);
            }
        }
        m->pos++;
        long long v = mx_binary(m, 1);
        if (!mx_is(m, ")")) {
            m->err = 1;
            return 0;
        }
        m->pos++;
        return v;
    }
    m->pos++;
    int ok = 0;
    long long v = 0;
    if (t[0] == '\'') {
        v = mx_char_literal(t, &ok);
    } else if (isdigit((unsigned char)t[0])) {
        v = mx_int_literal(t, &ok);
    } else if (isalpha((unsigned char)t[0]) || t[0] == '_') {
        ok = const_get(t, &v);
    }
    if (!ok)
        m->err = 1;
    return v;
}

static long long mx_binary(mx_t *m, int minprec) {
    long long l = mx_unary(m);
    while (!m->err && m->pos < m->n) {
        const char *op = m->t[m->pos];
        int p = mx_prec(op);
        if (!p || p < minprec)
            break;
        m->pos++;
        long long r = mx_binary(m, p + 1);
        unsigned long long a = (unsigned long long)l, b = (unsigned long long)r;
        switch (op[0]) {
            case '|': l = (long long)(a | b); break;
            case '^': l = (long long)(a ^ b); break;
            case '&': l = (long long)(a & b); break;
            case '+': l = (long long)(a + b); break;
            case '-': l = (long long)(a - b); break;
            case '*': l = (long long)(a * b); break;
            case '/': if (!r) m->err = 1; else l = l / r; break;
            case '%': if (!r) m->err = 1; else l = l % r; break;
            case '<':
                if (r < 0 || r >= 64) m->err = 1; else l = (long long)(a << r);
                break;
            case '>':
                if (r < 0 || r >= 64) m->err = 1; else l = l >> r;
                break;
        }
    }
    return l;
}

/* "( X )" -> "X" only when the first paren closes at the very end. */
static void mx_strip_parens(const char ***t, unsigned *n) {
    while (*n >= 3 && !strcmp((*t)[0], "(") && !strcmp((*t)[*n - 1], ")")) {
        int depth = 0, whole = 1;
        for (unsigned i = 0; i < *n; i++) {
            if (!strcmp((*t)[i], "(")) depth++;
            else if (!strcmp((*t)[i], ")")) depth--;
            if (depth == 0 && i + 1 < *n) {
                whole = 0;
                break;
            }
        }
        if (!whole)
            break;
        (*t)++;
        (*n) -= 2;
    }
}

static int mx_float_text(const char *tok, char *out, size_t cap) {
    size_t len = strlen(tok);
    if (len == 0 || len >= 64 || (!isdigit((unsigned char)tok[0]) && tok[0] != '.'))
        return 0;
    char buf[64];
    memcpy(buf, tok, len + 1);
    int hex = len > 1 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X');
    if (hex)
        return 0;
    while (len && strchr("fFlL", buf[len - 1]))
        buf[--len] = 0;
    if (!strchr(buf, '.') && !strpbrk(buf, "eE"))
        return 0;
    char *end;
    double v = strtod(buf, &end);
    if (*end)
        return 0;
    for (int prec = 6; prec <= 17; prec++) {
        snprintf(out, cap, "%.*g", prec, v);
        if (strtod(out, NULL) == v)
            break;
    }
    if (strpbrk(out, "eEn"))   /* exponent / inf / nan forms: leave to the user */
        return 0;
    if (!strchr(out, '.'))
        strncat(out, ".0", cap - strlen(out) - 1);
    return 1;
}

/* out == NULL: only evaluate, so later macros (AF_INET -> PF_INET) can refer to it. */
static void emit_macro(CXTranslationUnit tu, CXCursor c, sb_t *out) {
#if STBIND_EMIT_MACROS
    if (clang_Cursor_isMacroFunctionLike(c) || clang_Cursor_isMacroBuiltin(c))
        return;
    char *name = cxstr(clang_getCursorSpelling(c));
    if (!name[0] || is_reserved(name) || (name[0] == '_' && name[1] == '_'))
        return;
    if (hset_has(&g_macro_names, name))
        return;

    CXFile file;
    clang_getExpansionLocation(clang_getCursorLocation(c), &file, NULL, NULL, NULL);
    if (!file)
        return;
    char *fname = cxstr(clang_getFileName(file));
    if (!fname[0] || fname[0] == '<')
        return;

    CXSourceRange range = clang_getCursorExtent(c);
    unsigned end_off = 0;
    clang_getSpellingLocation(clang_getRangeEnd(range), NULL, NULL, NULL, &end_off);

    CXToken *toks = NULL;
    unsigned ntoks = 0;
    clang_tokenize(tu, range, &toks, &ntoks);
    if (!toks)
        return;

    const char **val = stbind_arena_push(sizeof(*val) * (ntoks + 1));
    int *is_str = stbind_arena_push(sizeof(*is_str) * (ntoks + 1));
    unsigned nval = 0;
    for (unsigned i = 1; i < ntoks; i++) {
        unsigned start_off = 0;
        clang_getSpellingLocation(clang_getRangeStart(clang_getTokenExtent(tu, toks[i])),
                                  NULL, NULL, NULL, &start_off);
        if (end_off && start_off >= end_off)   /* some libclang versions tokenize one extra */
            break;
        if (clang_getTokenKind(toks[i]) == CXToken_Comment)
            continue;
        val[nval] = cxstr(clang_getTokenSpelling(tu, toks[i]));
        is_str[nval] = clang_getTokenKind(toks[i]) == CXToken_Literal && val[nval][0] == '"';
        nval++;
    }
    clang_disposeTokens(tu, toks, ntoks);
    if (!nval)
        return;

    unsigned n = nval;
    const char **t = val;
    mx_strip_parens(&t, &n);
    if (!n)
        return;

    /* string literal (adjacent literals are concatenated) */
    int all_str = 1;
    for (unsigned i = 0; i < n; i++)
        if (!is_str[i + (unsigned)(t - val)])
            all_str = 0;
    if (all_str) {
        if (!out)
            return;
        sb_t sv;
        sb_init(&sv);
        sb_append(&sv, "\"");
        for (unsigned i = 0; i < n; i++) {
            size_t l = strlen(t[i]);
            if (l < 2) {
                free(sv.data);
                return;
            }
            char *inner = stbind_arena_push(l);
            memcpy(inner, t[i] + 1, l - 2);
            inner[l - 2] = 0;
            sb_append(&sv, inner);
        }
        sb_append(&sv, "\"");
        hset_add(&g_macro_names, name, clang_getNullCursor());
        sb_appendf(out, "%s :: %s;\n", name, sv.data);
        free(sv.data);
        return;
    }

    /* floating point literal, optionally negated */
    {
        int neg = n == 2 && !strcmp(t[0], "-");
        if (out && (n == 1 || neg)) {
            char fbuf[80];
            if (mx_float_text(t[neg ? 1 : 0], fbuf, sizeof(fbuf))) {
                hset_add(&g_macro_names, name, clang_getNullCursor());
                sb_appendf(out, "%s :: %s%s;\n", name, neg ? "-" : "", fbuf);
                return;
            }
        }
    }

    /* integer constant expression over literals and earlier constants */
    mx_t m = {.t = t, .n = n, .pos = 0, .err = 0};
    long long v = mx_binary(&m, 1);
    if (m.err || m.pos != m.n)
        return;

    hset_add(&g_macro_names, name, clang_getNullCursor());
    const_add(name, v);
    if (!out)
        return;
    if (v >= INT32_MIN && v <= INT32_MAX)
        sb_appendf(out, "%s :: %lld;\n", name, v);
    else if (v >= 0 && v <= (long long)UINT32_MAX)
        sb_appendf(out, "%s :: cast(u32) %lld;\n", name, v);
    else
        sb_appendf(out, "%s :: cast(i64) %lld;\n", name, v);
#else
    (void)tu; (void)c; (void)out;
#endif
}

/* STBIND_PUB: prefix every top-level declaration line with `pub `. */
static char *apply_pub(char *src) {
    sb_t o;
    sb_init(&o);
    char *p = src;
    while (*p) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        int decl = 0;
        if (len && p[0] != ' ' && p[0] != '/' && p[0] != '}' && p[0] != '\t') {
            static const char *const kw[] = {"struct ", "enum ", "enum_flag ", "extern ",
                                             "fn ", "using ", NULL};
            for (int i = 0; kw[i]; i++)
                if (!strncmp(p, kw[i], strlen(kw[i])))
                    decl = 1;
            if (!decl && (isalpha((unsigned char)p[0]) || p[0] == '_')) {
                size_t j = 0;
                while (j < len && (isalnum((unsigned char)p[j]) || p[j] == '_'))
                    j++;
                if (j + 4 <= len && !strncmp(p + j, " :: ", 4))
                    decl = 1;
            }
        }
        if (decl)
            sb_append(&o, "pub ");
        char *line = stbind_arena_push(len + 2);
        memcpy(line, p, len);
        line[len] = 0;
        sb_append(&o, line);
        if (e)
            sb_append(&o, "\n");
        if (!e)
            break;
        p = e + 1;
    }
    return o.data;
}

/*
 * Which headers do we bind?  The input header and any non-system header always.
 * With deep scope (default) also the system headers it pulls in (sys/socket.h ->
 * bits/socket.h, asm/socket.h ...), except the base headers below.
 * Per-run overrides: -stbind-include=SUBSTR  -stbind-exclude=SUBSTR
 *                    -stbind-shallow         -stbind-deep
 */
static const char *const g_base_headers[] = {
    "/lib/clang/", "/lib/gcc/", "/stddef.h", "/stdarg.h", "/stdint.h", "/stdbool.h",
    "/float.h", "/limits.h", "/syslimits.h", "/features.h", "/features-time64.h",
    "/sys/cdefs.h", "/sys/types.h", "/gnu/stubs", "/stdc-predef.h",
    "/bits/types.h", "/bits/types/", "/bits/typesizes.h", "/bits/wordsize.h",
    "/bits/libc-header-start.h", "/bits/long-double.h", "/bits/floatn",
    "/bits/stdint-", "/bits/time64.h", "/bits/byteswap.h", "/bits/uintn-identity.h",
    "/bits/endian.h", "/bits/endianness.h", "/endian.h", "/bits/local_lim.h",
    "/bits/posix1_lim.h", "/bits/posix2_lim.h", "/bits/xopen_lim.h",
    "/bits/pthreadtypes", "/bits/thread-shared-types.h", "/bits/struct_mutex.h",
    "/bits/struct_rwlock.h", "/bits/setjmp.h", "/bits/stdlib-float.h", NULL
};

static int file_in_scope(const char *path, int is_system) {
    for (int i = 0; i < g_n_excl; i++)
        if (strstr(path, g_excl[i]))
            return 0;
    for (int i = 0; i < g_n_incl; i++)
        if (strstr(path, g_incl[i]))
            return 1;
    if (!is_system)
        return 1;
    if (!g_deep)
        return 0;
    for (int i = 0; g_base_headers[i]; i++)
        if (strstr(path, g_base_headers[i]))
            return 0;
    return 1;
}

static int cursor_in_scope(CXSourceLocation loc) {
    CXFile file = NULL;
    clang_getExpansionLocation(loc, &file, NULL, NULL, NULL);
    if (!file)
        return 0;   /* builtin / command-line declarations */
    char *path = cxstr(clang_getFileName(file));
    if (!path[0] || path[0] == '<')
        return 0;
    if (g_scope_cache.cap) {
        hset_entry_t *e = hset_find_slot(g_scope_cache.slots, g_scope_cache.cap, path);
        if (e->used)
            return (int)e->ival;
    }
    int r = file_in_scope(path, clang_Location_isInSystemHeader(loc));
    hset_add(&g_scope_cache, path, clang_getNullCursor());
    hset_find_slot(g_scope_cache.slots, g_scope_cache.cap, path)->ival = r;
    return r;
}

/* Values of enumerators we do not emit still feed `#define X Y` evaluation. */
static void enum_register_consts(CXCursor d) {
    cursors_t ch = get_children(d);
    for (unsigned i = 0; i < ch.count; i++)
        if (clang_getCursorKind(ch.items[i]) == CXCursor_EnumConstantDecl)
            const_add(cxstr(clang_getCursorSpelling(ch.items[i])),
                      clang_getEnumConstantDeclValue(ch.items[i]));
    free_children(&ch);
}

static int enum_is_truly_anonymous(CXCursor d) {
    char *t = cxstr(clang_getTypeSpelling(clang_getCursorType(d)));
    return !t[0] || strstr(t, "unnamed") || strstr(t, "anonymous");
}

/* `enum { FOO = 1, BAR = 2 };` is the C idiom for constants: emit them as `FOO :: 1;`. */
static void emit_anon_enum_consts(CXCursor d, sb_t *out) {
    cursors_t ch = get_children(d);
    for (unsigned i = 0; i < ch.count; i++) {
        if (clang_getCursorKind(ch.items[i]) != CXCursor_EnumConstantDecl)
            continue;
        char *nm = cxstr(clang_getCursorSpelling(ch.items[i]));
        if (!nm[0] || is_reserved(nm) || (nm[0] == '_' && nm[1] == '_') ||
            hset_has(&g_macro_names, nm))
            continue;
        hset_add(&g_macro_names, nm, clang_getNullCursor());
        sb_appendf(out, "%s :: %lld;\n", nm, clang_getEnumConstantDeclValue(ch.items[i]));
    }
    free_children(&ch);
}

typedef struct {
    sb_t *out;
    sb_t *macros;
    CXTranslationUnit tu;
    CXFile main_file;
} visit_ctx_t;

static enum CXChildVisitResult top_level_visit(CXCursor c, CXCursor parent, CXClientData data) {
    (void)parent;
    visit_ctx_t *ctx = (visit_ctx_t *)data;
    enum CXCursorKind k = clang_getCursorKind(c);

    if ((k == CXCursor_StructDecl || k == CXCursor_UnionDecl || k == CXCursor_EnumDecl) &&
        clang_isCursorDefinition(c) && !decl_is_anonymous(c)) {
        char *nm = cxstr(clang_getCursorSpelling(c));
        if (nm[0])
            hset_add(&g_record_index, nm, c);
    }

    if (k == CXCursor_TypedefDecl) {
        CXType u = clang_getCanonicalType(clang_getTypedefDeclUnderlyingType(c));
        if ((u.kind == CXType_Record || u.kind == CXType_Enum) &&
            decl_is_anonymous(clang_getTypeDeclaration(u))) {
            char *tn = cxstr(clang_getCursorSpelling(c));
            if (tn[0])
                hset_add(&g_typedef_index, tn, c);
        }
    }

    CXSourceLocation loc = clang_getCursorLocation(c);
    if (!cursor_in_scope(loc)) {
        if (k == CXCursor_EnumDecl && clang_isCursorDefinition(c))
            enum_register_consts(c);
        else if (k == CXCursor_MacroDefinition)
            emit_macro(ctx->tu, c, NULL);
        return CXChildVisit_Continue;
    }

    if (k == CXCursor_FunctionDecl) {
        if (clang_isCursorDefinition(c)) {
            sb_t attempt;
            sb_init(&attempt);
            fail_reset();
            if (try_emit_full_fn(c, &attempt)) {
                emit_doc(c, ctx->out, 0);
                sb_append(ctx->out, attempt.data);
                sb_append(ctx->out, "\n");
            } else {
                char *name = cxstr(clang_getCursorSpelling(c));
                fprintf(stderr, "stbind: could not transpile body of '%s' (%s), emitting extern instead\n",
                        name, g_fail_set ? g_fail.data : "unknown reason");
                emit_extern_fn(c, ctx->out);
                sb_append(ctx->out, "\n");
            }
            free(attempt.data);
        } else {
            emit_extern_fn(c, ctx->out);
            sb_append(ctx->out, "\n");
        }
    } else if (k == CXCursor_StructDecl || k == CXCursor_UnionDecl || k == CXCursor_EnumDecl) {
        emit_record(c, ctx->out);
        if (k == CXCursor_EnumDecl && clang_isCursorDefinition(c)) {
            enum_register_consts(c);
            if (decl_is_anonymous(c) && enum_is_truly_anonymous(c))
                emit_anon_enum_consts(c, ctx->macros);
        }
    } else if (k == CXCursor_TypedefDecl) {
        emit_typedef(c, ctx->out);
    } else if (k == CXCursor_VarDecl) {
        emit_extern_var(c, ctx->out);
    } else if (k == CXCursor_MacroDefinition) {
        emit_macro(ctx->tu, c, ctx->macros);
    }

    return CXChildVisit_Continue;
}

static char *get_resource_dir(void) {
    FILE *p = popen("clang -print-resource-dir 2>/dev/null", "r");
    if (!p)
        return NULL;
    char buf[1024];
    char *r = fgets(buf, sizeof(buf), p);
    pclose(p);
    if (!r)
        return NULL;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    if (n == 0)
        return NULL;
    return strdup(buf);
}

static void emit_complex_support(sb_t *out, const char *sname, const char *prefix, const char *scal) {
    sb_appendf(out, "struct %s {\n    re: %s;\n    im: %s;\n}\n\n", sname, scal, scal);
    sb_appendf(out, "fn %s_from_real(r: %s) -> %s {\n    return %s { re = r, im = 0 };\n}\n\n",
              prefix, scal, sname, sname);
    sb_appendf(out, "fn %s_from_imag(i: %s) -> %s {\n    return %s { re = 0, im = i };\n}\n\n",
              prefix, scal, sname, sname);
    sb_appendf(out, "fn %s_add(a: %s, b: %s) -> %s {\n    return %s { re = (a.re + b.re), im = (a.im + b.im) };\n}\n\n",
              prefix, sname, sname, sname, sname);
    sb_appendf(out, "fn %s_sub(a: %s, b: %s) -> %s {\n    return %s { re = (a.re - b.re), im = (a.im - b.im) };\n}\n\n",
              prefix, sname, sname, sname, sname);
    sb_appendf(out, "fn %s_mul(a: %s, b: %s) -> %s {\n"
                    "    return %s { re = ((a.re * b.re) - (a.im * b.im)), im = ((a.re * b.im) + (a.im * b.re)) };\n}\n\n",
              prefix, sname, sname, sname, sname);
    sb_appendf(out, "fn %s_div(a: %s, b: %s) -> %s {\n"
                    "    denom: %s = ((b.re * b.re) + (b.im * b.im));\n"
                    "    return %s { re = (((a.re * b.re) + (a.im * b.im)) / denom), im = (((a.im * b.re) - (a.re * b.im)) / denom) };\n}\n\n",
              prefix, sname, sname, sname, scal, sname);
}


static void stbind_reset_state(void) {
    stbind_arena_reset();
    fail_reset();
    g_used_complex32 = 0;
    g_used_complex64 = 0;
    g_tmp_n = 0;
    g_cur_depth = 0;
    if (!g_prelude.data)
        sb_init(&g_prelude);
    else
        g_prelude.len = 0;
    memset(&g_needed_types, 0, sizeof(g_needed_types));
    memset(&g_opaque_types, 0, sizeof(g_opaque_types));
    memset(&g_emitted_types, 0, sizeof(g_emitted_types));
    memset(&g_record_index, 0, sizeof(g_record_index));
    memset(&g_consts, 0, sizeof(g_consts));
    memset(&g_macro_names, 0, sizeof(g_macro_names));
    memset(&g_typedef_index, 0, sizeof(g_typedef_index));
    memset(&g_scope_cache, 0, sizeof(g_scope_cache));
    g_deep = STBIND_DEEP_DEFAULT;
    g_n_incl = g_n_excl = 0;
}

char *stbind_generate_string(const char *header_path,
                             const char **extra_args, int n_extra_args,
                             char **err_msg) {
    if (err_msg)
        *err_msg = NULL;
    stbind_reset_state();

    char *resdir = get_resource_dir();
    char *resinc = NULL;
    if (resdir) {
        resinc = malloc(strlen(resdir) + 32);
        sprintf(resinc, "-isystem%s/include", resdir);
    }
    const char **clang_args = malloc(sizeof(char *) * (n_extra_args + 1));
    int n_clang_args = 0;
    if (resinc)
        clang_args[n_clang_args++] = resinc;
    for (int i = 0; i < n_extra_args; i++) {
        const char *a = extra_args[i];
        if (!strncmp(a, "-stbind-", 8)) {
            if (!strcmp(a, "-stbind-deep"))
                g_deep = 1;
            else if (!strcmp(a, "-stbind-shallow"))
                g_deep = 0;
            else if (!strncmp(a, "-stbind-include=", 16) && g_n_incl < 32)
                g_incl[g_n_incl++] = a + 16;
            else if (!strncmp(a, "-stbind-exclude=", 16) && g_n_excl < 32)
                g_excl[g_n_excl++] = a + 16;
            else
                fprintf(stderr, "stbind: unknown option '%s'\n", a);
            continue;
        }
        clang_args[n_clang_args++] = a;
    }

    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit tu;
    enum CXErrorCode err = clang_parseTranslationUnit2(
        index, header_path, clang_args, n_clang_args, NULL, 0,
        CXTranslationUnit_DetailedPreprocessingRecord, &tu);
    free(clang_args);
    free(resinc);
    free(resdir);
    if (err != CXError_Success) {
        if (err_msg) {
            char buf[512];
            snprintf(buf, sizeof(buf), "failed to parse '%s' (clang error %d)", header_path, err);
            *err_msg = strdup(buf);
        }
        clang_disposeIndex(index);
        return NULL;
    }

    unsigned n_diag = clang_getNumDiagnostics(tu);
    int had_error = 0;
    for (unsigned i = 0; i < n_diag; i++) {
        CXDiagnostic d = clang_getDiagnostic(tu, i);
        enum CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(d);
        if (sev >= CXDiagnostic_Error) {
            had_error = 1;
            char *msg = cxstr(clang_formatDiagnostic(d, clang_defaultDiagnosticDisplayOptions()));
            fprintf(stderr, "%s\n", msg);
        }
        clang_disposeDiagnostic(d);
    }
    if (had_error)
        fprintf(stderr, "stbind: '%s' had parse errors; output may be incomplete\n", header_path);

    sb_t out;
    sb_init(&out);

    sb_t macros_out;
    sb_init(&macros_out);
    visit_ctx_t ctx = { .out = &out, .macros = &macros_out, .tu = tu };
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, top_level_visit, &ctx);

    sb_t opaque_out;
    sb_init(&opaque_out);
    for (unsigned i = 0; i < g_opaque_types.cap; i++) {
	if (!g_opaque_types.slots[i].used)
	    continue;
	const char *nm = g_opaque_types.slots[i].key;
	if (hset_has(&g_record_index, nm))
	    continue;
	sb_appendf(&opaque_out, "using %s *void;\n", nm);
	hset_add(&g_emitted_types, nm, clang_getNullCursor());
    }

    sb_t deferred;
    sb_init(&deferred);
    for (int iter = 0; iter < 64; iter++) {
        int added = 0;
        for (unsigned i = 0; i < g_needed_types.cap; i++) {
            if (!g_needed_types.slots[i].used)
                continue;
            const char *nm = g_needed_types.slots[i].key;
            if (hset_has(&g_emitted_types, nm))
                continue;
            CXCursor *found = hset_find(&g_record_index, nm);
            if (found) {
                emit_record(*found, &deferred);
                added = 1;
                continue;
            }
            CXCursor *td = hset_find(&g_typedef_index, nm);
            if (td) {
                emit_typedef(*td, &deferred);
                hset_add(&g_emitted_types, nm, clang_getNullCursor());
                added = 1;
            }
        }
        if (!added)
            break;
    }

    sb_t final_out;
    sb_init(&final_out);
    if (g_used_complex32)
        emit_complex_support(&final_out, "Complex32", "complex32", "f32");
    if (g_used_complex64)
        emit_complex_support(&final_out, "Complex64", "complex64", "f64");
    if (macros_out.len)
        sb_append(&final_out, "\n");
    sb_append(&final_out, macros_out.data);
    if (macros_out.len)
        sb_append(&final_out, "\n");
    sb_append(&final_out, opaque_out.data);
    sb_append(&final_out, deferred.data);
    sb_append(&final_out, out.data);

    free(macros_out.data);
    free(opaque_out.data);
    free(deferred.data);
    free(out.data);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);

#if STBIND_PUB
    {
        char *pubbed = apply_pub(final_out.data);
        free(final_out.data);
        return pubbed;
    }
#else
    return final_out.data;
#endif
}

int stbind_generate(const char *header_path, const char *output_path,
                    const char **extra_args, int n_extra_args,
                    char **err_msg) {
    char *generated = stbind_generate_string(header_path, extra_args, n_extra_args, err_msg);
    if (!generated)
        return 0;

    FILE *f = fopen(output_path, "wb");
    if (!f) {
        if (err_msg) {
            char buf[512];
            snprintf(buf, sizeof(buf), "could not open '%s' for writing", output_path);
            *err_msg = strdup(buf);
        }
        free(generated);
        return 0;
    }
    fputs(generated, f);
    fclose(f);
    free(generated);
    return 1;
}

#endif /* STBIND_IMPLEMENTATION */
