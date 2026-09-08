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
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sb_append(sb, buf);
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
            else if (!strncmp(n, "union ", 6)) {
                if (!g_fail_set) {
                    g_fail_set = 1;
                    sb_appendf(&g_fail, "reference to union type '%s' (unions unsupported)", name);
                }
                return 0;
            }

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

static char *param_type_str(CXType t) {
    CXType canon = clang_getCanonicalType(t);
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
    sb_append(out, "(");
    int ok = emit_expr(operand, out);
    sb_appendf(out, " #as %s)", ty);
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
                sb_appendf(out, "%lld #as char", v);
            return 1;
        }
        case CXCursor_StringLiteral: {
            char *s = cxstr(clang_getCursorSpelling(e));
            sb_appendf(out, "%s", s);
            return 1;
        }
        case CXCursor_DeclRefExpr: {
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

static void emit_extern_fn(CXCursor f, sb_t *out) {
    char *name = cxstr(clang_getCursorSpelling(f));
    char *ret = type_str(clang_getResultType(clang_getCursorType(f)));
    sb_appendf(out, "extern fn %s(", name);
    int n = clang_Cursor_getNumArguments(f);
    for (int i = 0; i < n; i++) {
        if (i)
            sb_append(out, ", ");
        CXCursor arg = clang_Cursor_getArgument(f, i);
        char *aname = cxstr(clang_getCursorSpelling(arg));
        char *aty = param_type_str(clang_getCursorType(arg));
        if (!aname[0])
            sb_appendf(out, "a%d: %s", i, aty ? aty : "void");
        else
            sb_appendf(out, "%s: %s", aname, aty ? aty : "void");
    }
    if (clang_isFunctionTypeVariadic(clang_getCursorType(f)))
        sb_append(out, n ? ", ..." : "...");
    sb_appendf(out, ") -> %s;\n", ret ? ret : "void");
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
            sb_appendf(&body, "%s: %s", aname, aty);
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

static void emit_record(CXCursor d, sb_t *out) {
    if (clang_Cursor_isAnonymous(d))
        return;
    char *name = cxstr(clang_getCursorSpelling(d));
    if (!name[0]) {
        return;
    }

    if (hset_has(&g_emitted_types, name)) {
	return;
    }

    enum CXCursorKind k = clang_getCursorKind(d);
    if (k == CXCursor_EnumDecl) {
        sb_appendf(out, "enum %s {\n", name);
        cursors_t ch = get_children(d);
        for (unsigned i = 0; i < ch.count; i++) {
            if (clang_getCursorKind(ch.items[i]) != CXCursor_EnumConstantDecl)
                continue;
            char *ename = cxstr(clang_getCursorSpelling(ch.items[i]));
            long long v = clang_getEnumConstantDeclValue(ch.items[i]);
            sb_appendf(out, "    %s := %lld,\n", ename, v);
        }
        sb_append(out, "}\n\n");
        free_children(&ch);
        hset_add(&g_emitted_types, name, clang_getNullCursor());
        return;
    }
    if (k != CXCursor_StructDecl) {
        return;
    }
    if (!clang_isCursorDefinition(d)) {
        return;
    }
    hset_add(&g_emitted_types, name, clang_getNullCursor());
    cursors_t ch = get_children(d);
    sb_t body;
    sb_init(&body);
    int ok = 1;
    for (unsigned i = 0; i < ch.count; i++) {
        if (clang_getCursorKind(ch.items[i]) != CXCursor_FieldDecl)
            continue;
        char *fname = cxstr(clang_getCursorSpelling(ch.items[i]));
        CXType fty = clang_getCursorType(ch.items[i]);
        CXType fcanon = clang_getCanonicalType(fty);
        char *fty_str = NULL;

        if (fcanon.kind == CXType_Record &&
            clang_Cursor_isAnonymous(clang_getTypeDeclaration(fcanon))) {
            long long sz = clang_Type_getSizeOf(fcanon);
            if (sz > 0) {
                sb_t tmp;
                sb_init(&tmp);
                sb_appendf(&tmp, "[%lld]u8", sz);
                fty_str = stbind_arena_strdup(tmp.data);
                free(tmp.data);
            }
        }
        if (!fty_str)
            fty_str = type_str(fty);
        if (!fty_str) {
            ok = 0;
            break;
        }
        sb_appendf(&body, "    %s: %s;\n", fname, fty_str);
    }
    free_children(&ch);
    if (ok) {
        sb_appendf(out, "struct %s {\n%s}\n\n", name, body.data);
    } else {
        fprintf(stderr, "stbind: skipping struct '%s' (unsupported field type)\n", name);
    }
    free(body.data);
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

        if (same_name || clang_Cursor_isAnonymous(record_decl))
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


typedef struct {
    sb_t *out;
    CXFile main_file;
} visit_ctx_t;

static enum CXChildVisitResult top_level_visit(CXCursor c, CXCursor parent, CXClientData data) {
    (void)parent;
    visit_ctx_t *ctx = (visit_ctx_t *)data;
    enum CXCursorKind k = clang_getCursorKind(c);

    if ((k == CXCursor_StructDecl || k == CXCursor_EnumDecl) &&
        clang_isCursorDefinition(c) && !clang_Cursor_isAnonymous(c)) {
        char *nm = cxstr(clang_getCursorSpelling(c));
        if (nm[0])
            hset_add(&g_record_index, nm, c);
    }

    CXSourceLocation loc = clang_getCursorLocation(c);
    if (clang_Location_isInSystemHeader(loc))
        return CXChildVisit_Continue;

    if (k == CXCursor_FunctionDecl) {
        if (clang_isCursorDefinition(c)) {
            sb_t attempt;
            sb_init(&attempt);
            fail_reset();
            if (try_emit_full_fn(c, &attempt)) {
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
    } else if (k == CXCursor_StructDecl || k == CXCursor_EnumDecl) {
        emit_record(c, ctx->out);
    } else if (k == CXCursor_TypedefDecl) {
        emit_typedef(c, ctx->out);
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
    for (int i = 0; i < n_extra_args; i++)
        clang_args[n_clang_args++] = extra_args[i];

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

    visit_ctx_t ctx = { .out = &out };
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
            if (!found)
                continue;
            emit_record(*found, &deferred);
            added = 1;
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
    sb_append(&final_out, opaque_out.data);
    sb_append(&final_out, deferred.data);
    sb_append(&final_out, out.data);

    free(opaque_out.data);
    free(deferred.data);
    free(out.data);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);

    return final_out.data;
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
