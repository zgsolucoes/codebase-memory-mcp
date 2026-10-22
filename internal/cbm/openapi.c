/*
 * openapi.c -- OpenAPI and Swagger documents: each operation a Route, named by
 * the same QN as the route its handler registers in code (route_path.c), and
 * its `operationId` a reference to that handler.
 *
 * A Route is a definition of the document (its file DEFINES it). The
 * operationId is a doc-link record (CBM_DOCLINK_OPENAPI_OPERATION_ID) the
 * pipeline resolves as a qualified code name: Connexion's dotted paths
 * (`app.api.users.get_user`, or `x-openapi-router-controller` plus the id)
 * name one function; a bare id (`getUser`) names none for sure and stays
 * unresolved.
 *
 * The documents: `openapi.json` / `swagger.json` (CBM_LANG_OPENAPI, read with
 * the JSON grammar but no node per key), and any YAML or JSON file whose root
 * says `openapi:` or `swagger:` and has `paths`.
 */
#include "cbm.h"
#include "doclink.h"
#include "helpers.h" // cbm_node_text

#include "arena.h"
#include "foundation/constants.h"
#include "tree_sitter/api.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    OA_SCALAR_MAX = 1024, /* a key, a path, an operationId */
    OA_UNWRAP_MAX = 8,    /* stream > document > block_node > mapping */
    OA_METHOD_MAX = 8,
};

static bool oa_is(TSNode n, const char *type) {
    return !ts_node_is_null(n) && strcmp(ts_node_type(n), type) == 0;
}

/* The mapping a node holds (a YAML block or flow mapping, a JSON object),
 * through the wrappers around it; a null node for none. */
static TSNode oa_mapping(TSNode n) {
    for (int k = 0; k < OA_UNWRAP_MAX && !ts_node_is_null(n); k++) {
        if (oa_is(n, "block_mapping") || oa_is(n, "flow_mapping") || oa_is(n, "object")) {
            return n;
        }
        if (!oa_is(n, "stream") && !oa_is(n, "document") && !oa_is(n, "block_node") &&
            !oa_is(n, "flow_node")) {
            break;
        }
        TSNode next = {0};
        uint32_t nc = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < nc && ts_node_is_null(next); i++) {
            TSNode c = ts_node_named_child(n, i);
            if (!oa_is(c, "comment") && !oa_is(c, "anchor") && !oa_is(c, "tag")) {
                next = c;
            }
        }
        n = next;
    }
    return (TSNode){0};
}

static bool oa_pair(TSNode n) {
    return oa_is(n, "block_mapping_pair") || oa_is(n, "flow_pair") || oa_is(n, "pair");
}

/* A scalar's text (quotes dropped) into out: its length, 0 for none. */
static size_t oa_scalar(const CBMExtractCtx *ctx, TSNode n, char *out, size_t cap) {
    for (int k = 0; k < OA_UNWRAP_MAX && (oa_is(n, "flow_node") || oa_is(n, "block_node")); k++) {
        n = ts_node_named_child(n, 0);
    }
    if (ts_node_is_null(n) || !ts_node_is_null(oa_mapping(n)) || oa_is(n, "block_sequence") ||
        oa_is(n, "flow_sequence") || oa_is(n, "array")) {
        out[0] = '\0';
        return 0;
    }
    uint32_t b = ts_node_start_byte(n);
    uint32_t e = ts_node_end_byte(n);
    const char *s = ctx->source + b;
    size_t len = e - b;
    while (len && isspace((unsigned char)s[len - SKIP_ONE])) {
        len--;
    }
    if (len >= PAIR_LEN && (s[0] == '"' || s[0] == '\'') && s[len - SKIP_ONE] == s[0]) {
        s++;
        len -= PAIR_LEN;
    }
    if (len >= cap) {
        out[0] = '\0';
        return 0; /* longer than any key or reference read here */
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return len;
}

/* The value of `key` in a mapping; a null node for none. */
static TSNode oa_get(const CBMExtractCtx *ctx, TSNode map, const char *key) {
    char k[OA_SCALAR_MAX];
    uint32_t nc = ts_node_is_null(map) ? 0 : ts_node_named_child_count(map);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode p = ts_node_named_child(map, i);
        if (oa_pair(p) &&
            oa_scalar(ctx, ts_node_child_by_field_name(p, "key", CBM_SZ_3), k, sizeof(k)) &&
            strcmp(k, key) == 0) {
            return ts_node_child_by_field_name(p, "value", CBM_SZ_5);
        }
    }
    return (TSNode){0};
}

static const char *oa_dup_scalar(const CBMExtractCtx *ctx, TSNode n) {
    char v[OA_SCALAR_MAX];
    size_t len = oa_scalar(ctx, n, v, sizeof(v));
    return len ? cbm_arena_strndup(ctx->arena, v, len) : NULL;
}

/* Connexion's controller: on the operation, else its path. */
static const char *oa_controller(const CBMExtractCtx *ctx, TSNode op, TSNode path_item) {
    static const char *const keys[] = {"x-openapi-router-controller", "x-swagger-router-controller",
                                       NULL};
    for (int k = 0; keys[k]; k++) {
        const char *c = oa_dup_scalar(ctx, oa_get(ctx, op, keys[k]));
        c = c ? c : oa_dup_scalar(ctx, oa_get(ctx, path_item, keys[k]));
        if (c) {
            return c;
        }
    }
    return NULL;
}

/* The first item of a sequence (YAML block/flow sequence, JSON array),
 * through the wrappers around it; a null node for none. */
static TSNode oa_first_item(TSNode n) {
    for (int k = 0; k < OA_UNWRAP_MAX && !ts_node_is_null(n); k++) {
        if (oa_is(n, "block_sequence") || oa_is(n, "flow_sequence") || oa_is(n, "array")) {
            TSNode item = ts_node_named_child(n, 0);
            return oa_is(item, "block_sequence_item") ? ts_node_named_child(item, 0) : item;
        }
        if (!oa_is(n, "block_node") && !oa_is(n, "flow_node")) {
            break;
        }
        n = ts_node_named_child(n, 0);
    }
    return (TSNode){0};
}

/* The server's base path ("/api"): the path of the first OpenAPI 3 `servers`
 * url (after scheme and host), or Swagger 2 `basePath`; "" when it is the
 * root. A trailing slash is dropped. */
static void oa_base_path(const CBMExtractCtx *ctx, TSNode root, char *out, size_t cap) {
    char url[OA_SCALAR_MAX];
    out[0] = '\0';
    size_t len = oa_scalar(ctx, oa_get(ctx, root, "basePath"), url, sizeof(url));
    if (!len) {
        TSNode server = oa_mapping(oa_first_item(oa_get(ctx, root, "servers")));
        len = oa_scalar(ctx, oa_get(ctx, server, "url"), url, sizeof(url));
    }
    const char *p = url;
    const char *scheme = len ? strstr(url, "://") : NULL;
    if (scheme) {
        p = strchr(scheme + strlen("://"), '/');
    }
    if (!len || !p || *p != '/') {
        return;
    }
    size_t pl = strlen(p);
    while (pl > 0 && p[pl - SKIP_ONE] == '/') {
        pl--;
    }
    if (pl < cap) {
        memcpy(out, p, pl);
        out[pl] = '\0';
    }
}

/* One operation: its Route, and its operationId's reference. */
static void oa_operation(CBMExtractCtx *ctx, const char *path, const char *method, TSNode op,
                         TSNode path_item, TSNode pair) {
    CBMArena *a = ctx->arena;
    char upper[OA_METHOD_MAX];
    size_t ml = strlen(method);
    for (size_t i = 0; i < ml && i + SKIP_ONE < sizeof(upper); i++) {
        upper[i] = (char)toupper((unsigned char)method[i]);
        upper[i + SKIP_ONE] = '\0';
    }
    size_t pl = strlen(path);
    char *canon = (char *)cbm_arena_alloc(a, pl + SKIP_ONE);
    const char *qn = canon ? cbm_arena_sprintf(a, "__route__%s__%s", upper,
                                               cbm_route_canon_path(path, canon, pl + SKIP_ONE))
                           : NULL;
    const char *op_id = oa_dup_scalar(ctx, oa_get(ctx, op, "operationId"));
    const char *tag = oa_dup_scalar(ctx, oa_first_item(oa_get(ctx, op, "tags")));
    const char **kv = (const char **)cbm_arena_alloc(a, (CBM_SZ_8 + SKIP_ONE) * sizeof(char *));
    const char *m = cbm_arena_strdup(a, upper);
    if (!qn || !kv || !m) {
        return;
    }
    int k = 0;
    kv[k++] = "method";
    kv[k++] = m;
    kv[k++] = "source";
    kv[k++] = "openapi";
    if (op_id) {
        kv[k++] = "operation_id";
        kv[k++] = op_id;
    }
    if (tag) {
        kv[k++] = "tag";
        kv[k++] = tag;
    }
    kv[k] = NULL;
    CBMDefinition def;
    memset(&def, 0, sizeof(def));
    def.name = cbm_arena_strdup(a, path);
    def.qualified_name = qn;
    def.label = "Route";
    def.file_path = ctx->rel_path;
    def.start_line = ts_node_start_point(pair).row + SKIP_ONE;
    def.end_line = ts_node_end_point(pair).row + SKIP_ONE;
    def.extra_props = kv;
    if (!def.name) {
        return;
    }
    cbm_defs_push(&ctx->result->defs, a, def);
    if (!op_id) {
        return;
    }
    const char *controller = oa_controller(ctx, op, path_item);
    CBMDocLink link = {
        .source_qn = qn,
        .raw = controller ? cbm_arena_sprintf(a, "%s.%s", controller, op_id) : op_id,
        .line = def.start_line,
        .def_line = def.start_line,
        .syntax = CBM_DOCLINK_OPENAPI_OPERATION_ID,
    };
    if (link.raw) {
        cbm_doclinks_push(&ctx->result->doc_links, a, link);
    }
}

static bool oa_method(const char *k) {
    static const char *const methods[] = {"get",  "put",   "post",  "delete", "options",
                                          "head", "patch", "trace", NULL};
    for (int i = 0; methods[i]; i++) {
        if (strcmp(k, methods[i]) == 0) {
            return true;
        }
    }
    return false;
}

void cbm_extract_openapi(CBMExtractCtx *ctx) {
    if (!ctx || !ctx->result || !ctx->source || ts_node_is_null(ctx->root) ||
        (ctx->language != CBM_LANG_OPENAPI && ctx->language != CBM_LANG_YAML &&
         ctx->language != CBM_LANG_JSON)) {
        return;
    }
    TSNode root = oa_mapping(ctx->root);
    if (ts_node_is_null(oa_get(ctx, root, "openapi")) &&
        ts_node_is_null(oa_get(ctx, root, "swagger"))) {
        return; /* no API description */
    }
    TSNode paths = oa_mapping(oa_get(ctx, root, "paths"));
    char base[OA_SCALAR_MAX];
    oa_base_path(ctx, root, base, sizeof(base));
    char path[OA_SCALAR_MAX];
    char key[OA_SCALAR_MAX];
    uint32_t np = ts_node_is_null(paths) ? 0 : ts_node_named_child_count(paths);
    for (uint32_t i = 0; i < np; i++) {
        TSNode pp = ts_node_named_child(paths, i);
        if (!oa_pair(pp) ||
            !oa_scalar(ctx, ts_node_child_by_field_name(pp, "key", CBM_SZ_3), path, sizeof(path)) ||
            path[0] != '/') {
            continue;
        }
        TSNode item = oa_mapping(ts_node_child_by_field_name(pp, "value", CBM_SZ_5));
        char full[OA_SCALAR_MAX * PAIR_LEN];
        (void)snprintf(full, sizeof(full), "%s%s", base, path);
        uint32_t nm = ts_node_is_null(item) ? 0 : ts_node_named_child_count(item);
        for (uint32_t j = 0; j < nm; j++) {
            TSNode mp = ts_node_named_child(item, j);
            if (!oa_pair(mp) ||
                !oa_scalar(ctx, ts_node_child_by_field_name(mp, "key", CBM_SZ_3), key,
                           sizeof(key)) ||
                !oa_method(key)) {
                continue;
            }
            TSNode op = oa_mapping(ts_node_child_by_field_name(mp, "value", CBM_SZ_5));
            if (!ts_node_is_null(op)) {
                oa_operation(ctx, full, key, op, item, mp);
            }
        }
    }
}

/* ── Generated API client receivers ───────────────────────────────
 *
 * openapi-generator names a client after the operation's tag: <Tag>Api in
 * Java/Kotlin, <Tag>Service in typescript-angular, and the method after the
 * operationId. A call through a variable of that type (`faturamento.reviewFiles()`,
 * `this._userService.findUserByUsername()`) is a call to that operation,
 * whatever the variable is named. The file's declarations give the type:
 * fields, parameters (TypeScript constructor parameter properties included),
 * locals and `inject(Type)`.
 * shortcut: one name -> type table per file, not per scope; a name declared
 * with two client types is left out. */

enum { RT_MAX = 512, RT_NODE_BUDGET = 2000000 };
#define RT_FIELD(n, f) ts_node_child_by_field_name(n, f, (uint32_t)strlen(f))

typedef struct {
    const char *name[RT_MAX];
    const char *type[RT_MAX]; /* "" = ambiguous */
    int count;
} rt_table_t;

/* Last segment of a type's text, generics and array suffix dropped:
 * "com.x.BillingApi<T>[]" -> "BillingApi". */
static const char *rt_type_name(CBMExtractCtx *ctx, TSNode type) {
    char buf[OA_SCALAR_MAX];
    uint32_t b = ts_node_start_byte(type);
    uint32_t e = ts_node_end_byte(type);
    size_t n = 0;
    for (uint32_t i = b; i < e && n + 1 < sizeof(buf); i++) {
        char c = ctx->source[i];
        if (c == '<' || c == '[' || c == '|' || c == '(') {
            break;
        }
        if (c == '.') {
            n = 0;
        } else if (isalnum((unsigned char)c) || c == '_' || c == '$') {
            buf[n++] = c;
        }
    }
    buf[n] = '\0';
    return n ? cbm_arena_strndup(ctx->arena, buf, n) : NULL;
}

static bool rt_is_client_type(const char *t, CBMLanguage lang) {
    const char *suffix = (lang == CBM_LANG_TYPESCRIPT || lang == CBM_LANG_TSX) ? "Service" : "Api";
    size_t tl = t ? strlen(t) : 0;
    size_t sl = strlen(suffix);
    return tl > sl && isupper((unsigned char)t[0]) && strcmp(t + tl - sl, suffix) == 0;
}

static void rt_add(CBMExtractCtx *ctx, rt_table_t *t, TSNode name, const char *type) {
    if (ts_node_is_null(name) || !type || !rt_is_client_type(type, ctx->language)) {
        return;
    }
    const char *nm = cbm_node_text(ctx->arena, name, ctx->source);
    for (int i = 0; nm && i < t->count; i++) {
        if (strcmp(t->name[i], nm) == 0) {
            if (strcmp(t->type[i], type) != 0) {
                t->type[i] = "";
            }
            return;
        }
    }
    if (nm && t->count < RT_MAX) {
        t->name[t->count] = nm;
        t->type[t->count] = type;
        t->count++;
    }
}

/* The type of a TypeScript type_annotation (": UserService"). */
static const char *rt_ts_annotation(CBMExtractCtx *ctx, TSNode ann) {
    if (ts_node_is_null(ann)) {
        return NULL;
    }
    TSNode inner = oa_is(ann, "type_annotation") ? ts_node_named_child(ann, 0) : ann;
    return ts_node_is_null(inner) ? NULL : rt_type_name(ctx, inner);
}

/* `inject(UserService)` -> "UserService". */
static const char *rt_ts_inject(CBMExtractCtx *ctx, TSNode value) {
    if (!oa_is(value, "call_expression")) {
        return NULL;
    }
    TSNode fn = RT_FIELD(value, "function");
    TSNode args = RT_FIELD(value, "arguments");
    if (ts_node_is_null(fn) || ts_node_end_byte(fn) - ts_node_start_byte(fn) != strlen("inject") ||
        strncmp(ctx->source + ts_node_start_byte(fn), "inject", strlen("inject")) != 0 ||
        ts_node_is_null(args) || ts_node_named_child_count(args) == 0) {
        return NULL;
    }
    return rt_type_name(ctx, ts_node_named_child(args, 0));
}

static void rt_collect(CBMExtractCtx *ctx, TSNode n, rt_table_t *t) {
    const char *k = ts_node_type(n);
    TSNode type = RT_FIELD(n, "type");
    if (ctx->language == CBM_LANG_TYPESCRIPT || ctx->language == CBM_LANG_TSX) {
        if (strcmp(k, "required_parameter") == 0 || strcmp(k, "optional_parameter") == 0) {
            rt_add(ctx, t, RT_FIELD(n, "pattern"), rt_ts_annotation(ctx, type));
        } else if (strcmp(k, "public_field_definition") == 0 ||
                   strcmp(k, "variable_declarator") == 0) {
            TSNode name = RT_FIELD(n, "name");
            const char *ty = rt_ts_annotation(ctx, type);
            rt_add(ctx, t, name, ty ? ty : rt_ts_inject(ctx, RT_FIELD(n, "value")));
        }
        return;
    }
    if (ts_node_is_null(type)) {
        return;
    }
    const char *ty = rt_type_name(ctx, type);
    if (strcmp(k, "field_declaration") == 0 || strcmp(k, "local_variable_declaration") == 0) {
        uint32_t nc = ts_node_named_child_count(n); /* Java: one type, many declarators */
        for (uint32_t i = 0; i < nc; i++) {
            TSNode d = ts_node_named_child(n, i);
            if (oa_is(d, "variable_declarator")) {
                rt_add(ctx, t, RT_FIELD(d, "name"), ty);
            }
        }
    } else if (strcmp(k, "formal_parameter") == 0 || strcmp(k, "declaration") == 0 ||
               strcmp(k, "parameter") == 0) {
        rt_add(ctx, t, RT_FIELD(n, "name"), ty);
    }
}

/* Receiver of "x.op" / "this.x.op": the x segment, or NULL. */
static bool rt_receiver(const char *callee, char *out, size_t out_sz) {
    const char *op = callee ? strrchr(callee, '.') : NULL;
    if (!op || op == callee) {
        return false;
    }
    const char *recv = callee;
    if (strncmp(recv, "this.", strlen("this.")) == 0) {
        recv += strlen("this.");
    }
    size_t len = (size_t)(op - recv);
    if (len == 0 || len >= out_sz || memchr(recv, '.', len)) {
        return false;
    }
    memcpy(out, recv, len);
    out[len] = '\0';
    return true;
}

void cbm_extract_client_receiver_types(CBMExtractCtx *ctx) {
    if (!ctx || !ctx->result || ts_node_is_null(ctx->root) ||
        (ctx->language != CBM_LANG_JAVA && ctx->language != CBM_LANG_GROOVY &&
         ctx->language != CBM_LANG_KOTLIN && ctx->language != CBM_LANG_TYPESCRIPT &&
         ctx->language != CBM_LANG_TSX)) {
        return;
    }
    CBMCallArray *calls = &ctx->result->calls;
    bool any = false;
    for (int i = 0; i < calls->count && !any; i++) {
        any = calls->items[i].callee_name && strchr(calls->items[i].callee_name, '.');
    }
    if (!any) {
        return;
    }
    rt_table_t *t = (rt_table_t *)cbm_arena_alloc(ctx->arena, sizeof(rt_table_t));
    if (!t) {
        return;
    }
    t->count = 0;
    TSTreeCursor cur = ts_tree_cursor_new(ctx->root);
    int visited = 0;
    for (bool more = true; more && visited < RT_NODE_BUDGET; visited++) {
        rt_collect(ctx, ts_tree_cursor_current_node(&cur), t);
        if (ts_tree_cursor_goto_first_child(&cur)) {
            continue;
        }
        while (!ts_tree_cursor_goto_next_sibling(&cur)) {
            if (!ts_tree_cursor_goto_parent(&cur)) {
                more = false;
                break;
            }
        }
    }
    ts_tree_cursor_delete(&cur);
    for (int i = 0; i < calls->count && t->count > 0; i++) {
        char recv[OA_SCALAR_MAX];
        if (!rt_receiver(calls->items[i].callee_name, recv, sizeof(recv))) {
            continue;
        }
        for (int j = 0; j < t->count; j++) {
            if (t->type[j][0] && strcmp(t->name[j], recv) == 0) {
                calls->items[i].receiver_type = t->type[j];
                break;
            }
        }
    }
}
