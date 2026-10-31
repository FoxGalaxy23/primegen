#include "js_engine.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

struct JSFunction {
    char name[64];
    char* body;
};

typedef struct {
    const char* src;
    size_t pos;
    JSRuntime* rt;
} Parser;

typedef struct {
    JSValueType type;
    double number;
    char string[2048];
} JSValue;

static const char* EMPTY_STRING = "";

static void js_execute_script(JSRuntime* rt, const char* script, JSValue* ret_out, int allow_return);

static void js_safe_copy(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    /* Copy only the bytes that fit, without strncpy's ambiguous truncation
       semantics or compiler diagnostics. memmove also permits overlapping input. */
    size_t n = 0;
    while (n < dst_size - 1 && src[n] != '\0') n++;
    memmove(dst, src, n);
    dst[n] = '\0';
}

static void js_value_make_null(JSValue* v) {
    v->type = JS_VALUE_NULL;
    v->number = 0.0;
    v->string[0] = '\0';
}

static void js_value_make_number(JSValue* v, double n) {
    v->type = JS_VALUE_NUMBER;
    v->number = n;
    v->string[0] = '\0';
}

static void js_value_make_string(JSValue* v, const char* s) {
    v->type = JS_VALUE_STRING;
    v->number = 0.0;
    js_safe_copy(v->string, sizeof(v->string), s ? s : "");
}

static void js_value_to_string(const JSValue* v, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!v) {
        out[0] = '\0';
        return;
    }
    switch (v->type) {
        case JS_VALUE_STRING:
            js_safe_copy(out, out_size, v->string);
            break;
        case JS_VALUE_NUMBER:
            snprintf(out, out_size, "%.15g", v->number);
            break;
        default:
            js_safe_copy(out, out_size, "null");
            break;
    }
}

static double js_value_to_number(const JSValue* v) {
    if (!v) return 0.0;
    if (v->type == JS_VALUE_NUMBER) return v->number;
    if (v->type == JS_VALUE_STRING) return atof(v->string);
    return 0.0;
}

static void js_runtime_ensure_var_capacity(JSRuntime* rt) {
    if (rt->var_count < rt->var_capacity) return;
    int new_capacity = rt->var_capacity * 2;
    JSVariable* new_vars = (JSVariable*)realloc(rt->vars, sizeof(JSVariable) * (size_t)new_capacity);
    if (!new_vars) return;
    rt->vars = new_vars;
    rt->var_capacity = new_capacity;
}

static void js_runtime_ensure_func_capacity(JSRuntime* rt) {
    if (rt->func_count < rt->func_capacity) return;
    int new_capacity = rt->func_capacity * 2;
    JSFunction* new_funcs = (JSFunction*)realloc(rt->funcs, sizeof(JSFunction) * (size_t)new_capacity);
    if (!new_funcs) return;
    rt->funcs = new_funcs;
    rt->func_capacity = new_capacity;
}

static int js_find_var_index(JSRuntime* rt, const char* name) {
    if (!rt || !name) return -1;
    for (int i = 0; i < rt->var_count; ++i) {
        if (strcmp(rt->vars[i].name, name) == 0) return i;
    }
    return -1;
}

static const JSVariable* js_find_var(JSRuntime* rt, const char* name) {
    int idx = js_find_var_index(rt, name);
    if (idx < 0) return NULL;
    return &rt->vars[idx];
}

static int js_set_var_internal(JSRuntime* rt, const char* name, JSValue value, int is_const) {
    if (!rt || !name || !*name) return 0;

    int idx = js_find_var_index(rt, name);
    if (idx >= 0) {
        if (rt->vars[idx].is_const) {
            fprintf(stderr, "[JS Engine] Cannot reassign constant '%s'\n", name);
            return 0;
        }
        rt->vars[idx].type = value.type;
        rt->vars[idx].number = value.number;
        js_safe_copy(rt->vars[idx].string, sizeof(rt->vars[idx].string), value.string);
        if (is_const) rt->vars[idx].is_const = 1;
        return 1;
    }

    js_runtime_ensure_var_capacity(rt);
    if (rt->var_count >= rt->var_capacity) return 0;

    JSVariable* var = &rt->vars[rt->var_count++];
    memset(var, 0, sizeof(*var));
    js_safe_copy(var->name, sizeof(var->name), name);
    var->type = value.type;
    var->number = value.number;
    js_safe_copy(var->string, sizeof(var->string), value.string);
    var->is_const = is_const;
    return 1;
}

static int js_find_func_index(JSRuntime* rt, const char* name) {
    if (!rt || !name) return -1;
    for (int i = 0; i < rt->func_count; ++i) {
        if (strcmp(rt->funcs[i].name, name) == 0) return i;
    }
    return -1;
}

static void parser_skip_ws(Parser* p) {
    while (p->src[p->pos]) {
        if (isspace((unsigned char)p->src[p->pos])) {
            p->pos++;
            continue;
        }
        if (p->src[p->pos] == '/' && p->src[p->pos + 1] == '/') {
            p->pos += 2;
            while (p->src[p->pos] && p->src[p->pos] != '\n') p->pos++;
            continue;
        }
        if (p->src[p->pos] == '/' && p->src[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->src[p->pos] && !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/')) p->pos++;
            if (p->src[p->pos]) p->pos += 2;
            continue;
        }
        break;
    }
}

static int parser_read_identifier(Parser* p, char* out, size_t out_size) {
    parser_skip_ws(p);
    size_t start = p->pos;
    char c = p->src[p->pos];
    if (!(isalpha((unsigned char)c) || c == '_' || c == '$')) return 0;
    p->pos++;
    while ((c = p->src[p->pos])) {
        if (isalnum((unsigned char)c) || c == '_' || c == '$') p->pos++;
        else break;
    }
    size_t len = p->pos - start;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p->src + start, len);
    out[len] = '\0';
    return 1;
}

static void parser_read_string_literal(Parser* p, char* out, size_t out_size) {
    parser_skip_ws(p);
    char quote = p->src[p->pos];
    if (quote != '"' && quote != '\'') {
        out[0] = '\0';
        return;
    }
    p->pos++;
    size_t j = 0;
    while (p->src[p->pos] && p->src[p->pos] != quote) {
        char c = p->src[p->pos++];
        if (c == '\\' && p->src[p->pos]) {
            char esc = p->src[p->pos++];
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '\'': c = '\''; break;
                case '"': c = '"'; break;
                default: c = esc; break;
            }
        }
        if (j + 1 < out_size) out[j++] = c;
    }
    if (p->src[p->pos] == quote) p->pos++;
    out[j] = '\0';
}

static JSValue parse_expression(Parser* p);

static JSValue make_from_identifier(Parser* p, const char* name) {
    JSValue v;
    js_value_make_null(&v);

    if (strcmp(name, "null") == 0 || strcmp(name, "undefined") == 0) return v;
    if (strcmp(name, "true") == 0) {
        js_value_make_number(&v, 1.0);
        return v;
    }
    if (strcmp(name, "false") == 0) {
        js_value_make_number(&v, 0.0);
        return v;
    }
    if (strcmp(name, "Math.PI") == 0) {
        js_value_make_number(&v, 3.14159265358979323846);
        return v;
    }
    if (strcmp(name, "Math.E") == 0) {
        js_value_make_number(&v, 2.71828182845904523536);
        return v;
    }
    if (strcmp(name, "Math.random") == 0) {
        // simple random for now
        js_value_make_number(&v, (double)rand() / (double)RAND_MAX);
        return v;
    }

    const JSVariable* var = js_find_var(p->rt, name);
    if (!var) {
        fprintf(stderr, "[JS Engine] Warning: unknown variable '%s'\n", name);
        return v;
    }

    v.type = var->type;
    v.number = var->number;
    js_safe_copy(v.string, sizeof(v.string), var->string);
    return v;
}

static JSValue parse_factor(Parser* p) {
    parser_skip_ws(p);

    JSValue v;
    js_value_make_null(&v);

    char c = p->src[p->pos];
    if (c == '\0') return v;

    if (c == '(') {
        p->pos++;
        v = parse_expression(p);
        parser_skip_ws(p);
        if (p->src[p->pos] == ')') p->pos++;
        return v;
    }

    if (c == '+' || c == '-') {
        p->pos++;
        v = parse_factor(p);
        if (c == '-' && v.type == JS_VALUE_NUMBER) {
            v.number = -v.number;
        } else if (c == '-' && v.type == JS_VALUE_STRING) {
            v.number = -atof(v.string);
            v.type = JS_VALUE_NUMBER;
            v.string[0] = '\0';
        }
        return v;
    }

    if (c == '"' || c == '\'') {
        char buffer[256];
        parser_read_string_literal(p, buffer, sizeof(buffer));
        js_value_make_string(&v, buffer);
        return v;
    }

    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)p->src[p->pos + 1]))) {
        char* endptr = NULL;
        double num = strtod(p->src + p->pos, &endptr);
        p->pos = (size_t)(endptr - p->src);
        js_value_make_number(&v, num);
        return v;
    }

    char ident[128];
    if (parser_read_identifier(p, ident, sizeof(ident))) {
        parser_skip_ws(p);
        if (p->src[p->pos] == '(') {
            p->pos++;
            char args[256] = {0};
            int depth = 1;
            size_t start = p->pos;
            while (p->src[p->pos] && depth > 0) {
                if (p->src[p->pos] == '(') depth++;
                else if (p->src[p->pos] == ')') depth--;
                if (depth > 0) p->pos++;
            }
            size_t len = p->pos - start;
            if (len >= sizeof(args)) len = sizeof(args) - 1;
            memcpy(args, p->src + start, len);
            args[len] = '\0';
            if (p->src[p->pos] == ')') p->pos++;

            if (strcmp(ident, "setInterval") == 0) {
                // syntax: setInterval("code", delay)
                Parser ap = { .src = args, .pos = 0, .rt = p->rt };
                JSValue code_v = parse_expression(&ap);
                parser_skip_ws(&ap);
                if (ap.src[ap.pos] == ',') ap.pos++;
                JSValue delay_v = parse_expression(&ap);

                char code[512];
                js_value_to_string(&code_v, code, sizeof(code));
                int delay = (int)js_value_to_number(&delay_v);

                // Use a dummy variable to find a free interval slot in main.c logic
                // For simplicity, we'll store intervals in JS variables with a prefix
                char interval_key[64];
                static int g_interval_id_gen = 1;
                int id = g_interval_id_gen++;
                snprintf(interval_key, sizeof(interval_key), "__interval_%d", id);

                char val_str[1024];
                snprintf(val_str, sizeof(val_str), "%d|%s", delay, code);
                js_set_string(p->rt, interval_key, val_str);

                js_value_make_number(&v, (double)id);
                return v;
            }
            if (strcmp(ident, "clearInterval") == 0) {
                Parser ap = { .src = args, .pos = 0, .rt = p->rt };
                JSValue id_v = parse_expression(&ap);
                int id = (int)js_value_to_number(&id_v);
                char interval_key[64];
                snprintf(interval_key, sizeof(interval_key), "__interval_%d", id);

                // Remove it by setting to empty or null
                js_set_string(p->rt, interval_key, "");
                js_value_make_null(&v);
                return v;
            }

            if (strcmp(ident, "Math.sin") == 0) {
                Parser ap = { .src = args, .pos = 0, .rt = p->rt };
                JSValue av = parse_expression(&ap);
                js_value_make_number(&v, sin(js_value_to_number(&av)));
                return v;
            } else if (strcmp(ident, "Math.cos") == 0) {
                Parser ap = { .src = args, .pos = 0, .rt = p->rt };
                JSValue av = parse_expression(&ap);
                js_value_make_number(&v, cos(js_value_to_number(&av)));
                return v;
            } else if (strcmp(ident, "Math.floor") == 0) {
                Parser ap = { .src = args, .pos = 0, .rt = p->rt };
                JSValue av = parse_expression(&ap);
                js_value_make_number(&v, floor(js_value_to_number(&av)));
                return v;
            } else if (strcmp(ident, "Math.abs") == 0) {
                Parser ap = { .src = args, .pos = 0, .rt = p->rt };
                JSValue av = parse_expression(&ap);
                js_value_make_number(&v, fabs(js_value_to_number(&av)));
                return v;
            } else if (strcmp(ident, "Math.sqrt") == 0) {
                Parser ap = { .src = args, .pos = 0, .rt = p->rt };
                JSValue av = parse_expression(&ap);
                js_value_make_number(&v, sqrt(js_value_to_number(&av)));
                return v;
            } else {
                // User-defined function call
                int idx = js_find_func_index(p->rt, ident);
                if (idx >= 0) {
                    JSValue ret;
                    js_value_make_null(&ret);
                    // For now, no arguments supported for user-defined functions via setInterval
                    js_execute_script(p->rt, p->rt->funcs[idx].body, &ret, 1);
                    return ret;
                }
            }
        }
        if (p->src[p->pos] == '.') {
            size_t save = p->pos;
            p->pos++;
            char rhs[128];
            if (parser_read_identifier(p, rhs, sizeof(rhs))) {
                char full[256];
                snprintf(full, sizeof(full), "%s.%s", ident, rhs);
                return make_from_identifier(p, full);
            }
            p->pos = save;
        }
        return make_from_identifier(p, ident);
    }

    return v;
}

static JSValue parse_term(Parser* p) {
    JSValue left = parse_factor(p);
    while (1) {
        parser_skip_ws(p);
        char op = p->src[p->pos];
        if (op != '*' && op != '/' && op != '%') break;
        p->pos++;
        JSValue right = parse_factor(p);
        double a = js_value_to_number(&left);
        double b = js_value_to_number(&right);
        js_value_make_number(&left, 0.0);
        if (op == '*') left.number = a * b;
        else if (op == '/') left.number = (b == 0.0) ? 0.0 : a / b;
        else left.number = (b == 0.0) ? 0.0 : (double)((long long)a % (long long)b);
    }
    return left;
}

static JSValue parse_arithmetic_expr(Parser* p) {
    JSValue left = parse_term(p);
    while (1) {
        parser_skip_ws(p);
        char op = p->src[p->pos];
        if (op != '+' && op != '-') break;
        p->pos++;
        JSValue right = parse_term(p);
        if (op == '+' && (left.type == JS_VALUE_STRING || right.type == JS_VALUE_STRING)) {
            /* JSValue strings are capped at 2047 bytes. The old 256-byte
               temporary silently truncated both operands before concatenation. */
            char lhs_num[64];
            char rhs_text[sizeof(right.string)];
            const char* lhs_text = left.string;
            if (left.type != JS_VALUE_STRING) {
                js_value_to_string(&left, lhs_num, sizeof(lhs_num));
                lhs_text = lhs_num;
            }
            js_value_to_string(&right, rhs_text, sizeof(rhs_text));
            size_t lhs_len = strlen(lhs_text);
            size_t lhs_copy = lhs_len < sizeof(left.string) - 1 ? lhs_len : sizeof(left.string) - 1;
            if (lhs_text != left.string) memmove(left.string, lhs_text, lhs_copy);
            size_t room = sizeof(left.string) - 1 - lhs_copy;
            size_t rhs_len = strlen(rhs_text);
            size_t rhs_copy = rhs_len < room ? rhs_len : room;
            memcpy(left.string + lhs_copy, rhs_text, rhs_copy);
            left.string[lhs_copy + rhs_copy] = '\0';
            left.type = JS_VALUE_STRING;
            left.number = 0.0;
        } else {
            double a = js_value_to_number(&left);
            double b = js_value_to_number(&right);
            js_value_make_number(&left, (op == '+') ? a + b : a - b);
        }
    }
    return left;
}

// ДОБАВЛЕНО: Теперь выражения умеют обрабатывать операторы сравнения (==, <, >)
static JSValue parse_expression(Parser* p) {
    JSValue left = parse_arithmetic_expr(p);
    parser_skip_ws(p);

    char op = p->src[p->pos];
    if (op == '=' && p->src[p->pos + 1] == '=') {
        p->pos += 2;
        JSValue right = parse_arithmetic_expr(p);
        double res = (js_value_to_number(&left) == js_value_to_number(&right)) ? 1.0 : 0.0;
        js_value_make_number(&left, res);
    } else if (op == '<') {
        p->pos++;
        JSValue right = parse_arithmetic_expr(p);
        double res = (js_value_to_number(&left) < js_value_to_number(&right)) ? 1.0 : 0.0;
        js_value_make_number(&left, res);
    } else if (op == '>') {
        p->pos++;
        JSValue right = parse_arithmetic_expr(p);
        double res = (js_value_to_number(&left) > js_value_to_number(&right)) ? 1.0 : 0.0;
        js_value_make_number(&left, res);
    }
    return left;
}

static char* js_extract_block(const char* src, size_t open_brace_pos, size_t* end_pos) {
    int depth = 0;
    int in_string = 0;
    char quote = '\0';

    for (size_t i = open_brace_pos; src[i]; ++i) {
        char c = src[i];
        if (in_string) {
            if (c == '\\' && src[i + 1]) {
                ++i;
                continue;
            }
            if (c == quote) in_string = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_string = 1;
            quote = c;
            continue;
        }
        if (c == '/' && src[i + 1] == '/') {
            while (src[i] && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && src[i + 1] == '*') {
            i += 2;
            while (src[i] && !(src[i] == '*' && src[i + 1] == '/')) i++;
            if (src[i]) i++;
            continue;
        }
        if (c == '{') depth++;
        if (c == '}') {
            depth--;
            if (depth == 0) {
                size_t body_start = open_brace_pos + 1;
                size_t body_len = i - body_start;
                char* body = (char*)malloc(body_len + 1);
                if (!body) return NULL;
                memcpy(body, src + body_start, body_len);
                body[body_len] = '\0';
                if (end_pos) *end_pos = i + 1;
                return body;
            }
        }
    }
    return NULL;
}

void js_set_dom_callback(JSRuntime* rt, void (*cb)(void*, const char*, const char*), void* user_data) {
    if (!rt) return;
    rt->dom_set_value_cb = cb;
    rt->user_data = user_data;
}

static void js_execute_script(JSRuntime* rt, const char* script, JSValue* ret_out, int allow_return);

static void js_store_variable(JSRuntime* rt, const char* name, const JSValue* v, int is_const) {
    if (!v) return;
    js_set_var_internal(rt, name, *v, is_const);
}

static void js_execute_statement(JSRuntime* rt, const char* stmt, JSValue* ret_out, int allow_return) {
    if (!stmt) return;

    while (isspace((unsigned char)*stmt)) stmt++;
    if (!*stmt) return;

    // ДОБАВЛЕНО: Парсинг конструкции "for"
    if (strncmp(stmt, "for", 3) == 0 && (isspace((unsigned char)stmt[3]) || stmt[3] == '(')) {
        const char* lp = strchr(stmt, '(');
        const char* brace = strchr(stmt, '{');
        if (lp && brace && brace > lp) {
            // Разделяем заголовок цикла на init; cond; step
            char header[256];
            const char* rp = brace;
            while (rp > lp && *rp != ')') rp--;
            size_t hlen = (size_t)(rp - lp - 1);
            if (hlen >= sizeof(header)) hlen = sizeof(header) - 1;
            memcpy(header, lp + 1, hlen);
            header[hlen] = '\0';

            char* s1 = header;
            char* s2 = strchr(s1, ';');
            if (s2) {
                *s2 = '\0';
                char* s3 = strchr(s2 + 1, ';');
                if (s3) {
                    *s3 = '\0';
                    char* init = s1;
                    char* cond = s2 + 1;
                    char* step = s3 + 1;

                    size_t end_pos = 0;
                    char* body = js_extract_block(stmt, (size_t)(brace - stmt), &end_pos);
                    if (body) {
                        js_execute_statement(rt, init, NULL, 0);
                        while (1) {
                            Parser cp = { .src = cond, .pos = 0, .rt = rt };
                            JSValue cv = parse_expression(&cp);
                            if (js_value_to_number(&cv) == 0.0) break;

                            js_execute_script(rt, body, ret_out, allow_return);
                            if (ret_out && ret_out->type != JS_VALUE_NULL) break;
                            js_execute_statement(rt, step, NULL, 0);
                        }
                        free(body);
                    }
                }
            }
        }
        return;
    }

    // ДОБАВЛЕНО: Парсинг конструкции "if"
    if (strncmp(stmt, "if", 2) == 0 && (isspace((unsigned char)stmt[2]) || stmt[2] == '(')) {
        const char* lp = strchr(stmt, '(');
        const char* brace = strchr(stmt, '{');
        if (lp && brace && brace > lp) {
            // Выделяем выражение внутри ( )
            char cond_expr[256];
            const char* rp = brace;
            while (rp > lp && *rp != ')') rp--;

            size_t cond_len = (size_t)(rp - lp - 1);
            if (cond_len >= sizeof(cond_expr)) cond_len = sizeof(cond_expr) - 1;
            memcpy(cond_expr, lp + 1, cond_len);
            cond_expr[cond_len] = '\0';

            // Считаем значение условия
            Parser p = { .src = cond_expr, .pos = 0, .rt = rt };
            JSValue cond_val = parse_expression(&p);

            // Если условие истинно (не ноль), выполняем блок внутри { }
            if (js_value_to_number(&cond_val) != 0.0) {
                size_t end_pos = 0;
                char* body = js_extract_block(stmt, (size_t)(brace - stmt), &end_pos);
                if (body) {
                    js_execute_script(rt, body, ret_out, allow_return);
                    free(body);
                }
            }
        }
        return;
    }

    if (allow_return && strncmp(stmt, "return", 6) == 0 && (stmt[6] == '\0' || isspace((unsigned char)stmt[6]) || stmt[6] == '(')) {
        Parser p = { .src = stmt + 6, .pos = 0, .rt = rt };
        JSValue v = parse_expression(&p);
        if (ret_out) *ret_out = v;
        return;
    }

    if (strncmp(stmt, "console.log", 11) == 0) {
        const char* lp = strchr(stmt, '(');
        const char* rp = strrchr(stmt, ')');
        if (lp && rp && rp > lp) {
            char expr[512];
            size_t len = (size_t)(rp - lp - 1);
            if (len >= sizeof(expr)) len = sizeof(expr) - 1;
            memcpy(expr, lp + 1, len);
            expr[len] = '\0';
            Parser p = { .src = expr, .pos = 0, .rt = rt };
            JSValue v = parse_expression(&p);
            char out[256];
            js_value_to_string(&v, out, sizeof(out));
            printf("%s\n", out);
        }
        return;
    }

    if (strncmp(stmt, "let ", 4) == 0 || strncmp(stmt, "const ", 6) == 0 || strncmp(stmt, "var ", 4) == 0) {
        int is_const = (strncmp(stmt, "const ", 6) == 0);
        const char* p = stmt + (is_const ? 6 : 4);
        while (isspace((unsigned char)*p)) p++;
        char name[64];
        size_t ni = 0;
        if (!(isalpha((unsigned char)*p) || *p == '_' || *p == '$')) {
            fprintf(stderr, "[JS Engine] Invalid variable name in: %s\n", stmt);
            return;
        }
        while ((isalnum((unsigned char)*p) || *p == '_' || *p == '$') && ni + 1 < sizeof(name)) {
            name[ni++] = *p++;
        }
        name[ni] = '\0';

        JSValue value;
        js_value_make_null(&value);
        while (isspace((unsigned char)*p)) p++;
        if (*p == '=') {
            p++;
            Parser expr = { .src = p, .pos = 0, .rt = rt };
            value = parse_expression(&expr);
        }
        js_store_variable(rt, name, &value, is_const);
        return;
    }

    if (strncmp(stmt, "function ", 9) == 0) {
        const char* p = stmt + 9;
        while (isspace((unsigned char)*p)) p++;
        char fname[64];
        size_t fi = 0;
        if (!(isalpha((unsigned char)*p) || *p == '_' || *p == '$')) {
            fprintf(stderr, "[JS Engine] Invalid function name in: %s\n", stmt);
            return;
        }
        while ((isalnum((unsigned char)*p) || *p == '_' || *p == '$') && fi + 1 < sizeof(fname)) {
            fname[fi++] = *p++;
        }
        fname[fi] = '\0';

        const char* brace = strchr(stmt, '{');
        if (!brace) {
            fprintf(stderr, "[JS Engine] Missing function body for '%s'\n", fname);
            return;
        }
        size_t end_pos = 0;
        char* body = js_extract_block(stmt, (size_t)(brace - stmt), &end_pos);
        if (!body) {
            fprintf(stderr, "[JS Engine] Cannot parse function body for '%s'\n", fname);
            return;
        }

        int idx = js_find_func_index(rt, fname);
        if (idx >= 0) {
            free(rt->funcs[idx].body);
            rt->funcs[idx].body = body;
            return;
        }

        js_runtime_ensure_func_capacity(rt);
        if (rt->func_count >= rt->func_capacity) {
            free(body);
            return;
        }

        JSFunction* fn = &rt->funcs[rt->func_count++];
        memset(fn, 0, sizeof(*fn));
        js_safe_copy(fn->name, sizeof(fn->name), fname);
        fn->body = body;
        return;
    }

    {
        Parser check = { .src = stmt, .pos = 0, .rt = rt };
        char name[256]; // Larger buffer for document.form.element.value

        // Check for complex assignment like document.retroForm.display.value = ...
        if (strncmp(stmt, "document.", 9) == 0) {
            const char* eq = strchr(stmt, '=');
            if (eq && eq[1] != '=') {
                size_t path_len = (size_t)(eq - stmt);
                while (path_len > 0 && isspace((unsigned char)stmt[path_len-1])) path_len--;
                char path[256];
                if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
                memcpy(path, stmt, path_len);
                path[path_len] = '\0';

                // Extract only the part after "document."
                const char* actual_path = path + 9;

                Parser expr_p = { .src = eq + 1, .pos = 0, .rt = rt };
                JSValue val = parse_expression(&expr_p);
                char val_str[512];
                js_value_to_string(&val, val_str, sizeof(val_str));

                if (rt->dom_set_value_cb) {
                    rt->dom_set_value_cb(rt->user_data, actual_path, val_str);
                }
                return;
            }
        }

        if (parser_read_identifier(&check, name, sizeof(name))) {
            parser_skip_ws(&check);
            if (check.src[check.pos] == '=' && check.src[check.pos + 1] != '=') {
                check.pos++;
                JSValue value = parse_expression(&check);
                js_store_variable(rt, name, &value, 0);
                return;
            }
            // ДОБАВЛЕНО: Поддержка i++ и i--
            if (check.src[check.pos] == '+' && check.src[check.pos + 1] == '+') {
                const JSVariable* v = js_find_var(rt, name);
                if (v && v->type == JS_VALUE_NUMBER) {
                    js_set_number(rt, name, v->number + 1.0);
                }
                return;
            }
            if (check.src[check.pos] == '-' && check.src[check.pos + 1] == '-') {
                const JSVariable* v = js_find_var(rt, name);
                if (v && v->type == JS_VALUE_NUMBER) {
                    js_set_number(rt, name, v->number - 1.0);
                }
                return;
            }
        }
    }

    {
        Parser p = { .src = stmt, .pos = 0, .rt = rt };
        (void)parse_expression(&p);
    }
}

static void js_execute_script(JSRuntime* rt, const char* script, JSValue* ret_out, int allow_return) {
    if (!rt || !script) return;

    size_t i = 0;
    while (script[i]) {
        while (isspace((unsigned char)script[i])) i++;
        if (!script[i]) break;

        if (script[i] == '/' && script[i + 1] == '/') {
            while (script[i] && script[i] != '\n') i++;
            continue;
        }
        if (script[i] == '/' && script[i + 1] == '*') {
            i += 2;
            while (script[i] && !(script[i] == '*' && script[i + 1] == '/')) i++;
            if (script[i]) i += 2;
            continue;
        }

        // ДОБАВЛЕНО: Перехват конструкции if и for прямо на верхнем уровне обработки скрипта
        if (strncmp(script + i, "if", 2) == 0 && (isspace((unsigned char)script[i + 2]) || script[i + 2] == '(')) {
            size_t stmt_start = i;
            const char* brace = strchr(script + i, '{');
            if (!brace) break;
            size_t end_pos = 0;
            char* block = js_extract_block(script, (size_t)(brace - script), &end_pos);
            if (!block) break;

            size_t stmt_len = end_pos - stmt_start;
            char* stmt = (char*)malloc(stmt_len + 1);
            if (!stmt) {
                free(block);
                break;
            }
            memcpy(stmt, script + stmt_start, stmt_len);
            stmt[stmt_len] = '\0';

            js_execute_statement(rt, stmt, ret_out, allow_return);
            free(stmt);
            free(block);
            i = end_pos;
            continue;
        }

        if (strncmp(script + i, "for", 3) == 0 && (isspace((unsigned char)script[i + 3]) || script[i + 3] == '(')) {
            size_t stmt_start = i;
            const char* brace = strchr(script + i, '{');
            if (!brace) break;
            size_t end_pos = 0;
            char* block = js_extract_block(script, (size_t)(brace - script), &end_pos);
            if (!block) break;

            size_t stmt_len = end_pos - stmt_start;
            char* stmt = (char*)malloc(stmt_len + 1);
            if (!stmt) {
                free(block);
                break;
            }
            memcpy(stmt, script + stmt_start, stmt_len);
            stmt[stmt_len] = '\0';

            js_execute_statement(rt, stmt, ret_out, allow_return);
            free(stmt);
            free(block);
            i = end_pos;
            continue;
        }

        if (strncmp(script + i, "function", 8) == 0 && !isalnum((unsigned char)script[i + 8]) && script[i + 8] != '_') {
            size_t stmt_start = i;
            const char* brace = strchr(script + i, '{');
            if (!brace) break;
            size_t end_pos = 0;
            char* block = js_extract_block(script, (size_t)(brace - script), &end_pos);
            if (!block) break;
            size_t stmt_len = end_pos - stmt_start;
            char* stmt = (char*)malloc(stmt_len + 1);
            if (!stmt) {
                free(block);
                break;
            }
            memcpy(stmt, script + stmt_start, stmt_len);
            stmt[stmt_len] = '\0';
            js_execute_statement(rt, stmt, ret_out, allow_return);
            free(stmt);
            free(block);
            i = end_pos;
            if (allow_return && ret_out && ret_out->type != JS_VALUE_NULL) return;
            continue;
        }

        size_t start = i;
        int in_string = 0;
        char quote = '\0';
        int depth = 0;
        while (script[i]) {
            char c = script[i];
            if (in_string) {
                if (c == '\\' && script[i + 1]) {
                    i += 2;
                    continue;
                }
                if (c == quote) in_string = 0;
                i++;
                continue;
            }
            if (c == '"' || c == '\'') {
                in_string = 1;
                quote = c;
                i++;
                continue;
            }
            if (c == '(' || c == '[' || c == '{') {
                depth++;
                i++;
                continue;
            }
            if (c == ')' || c == ']' || c == '}') {
                if (depth > 0) depth--;
                i++;
                continue;
            }
            if ((c == ';' || c == '\n') && depth == 0) break;
            i++;
        }

        size_t len = i - start;
        char* stmt = (char*)malloc(len + 1);
        if (!stmt) return;
        memcpy(stmt, start + script, len);
        stmt[len] = '\0';
        js_execute_statement(rt, stmt, ret_out, allow_return);
        free(stmt);

        if (script[i] == ';' || script[i] == '\n') i++;
        if (allow_return && ret_out && ret_out->type != JS_VALUE_NULL) return;
    }
}

JSRuntime* js_create_runtime(void) {
    srand((unsigned int)time(NULL));
    JSRuntime* rt = (JSRuntime*)calloc(1, sizeof(JSRuntime));
    if (!rt) return NULL;

    rt->var_capacity = 16;
    rt->vars = (JSVariable*)calloc((size_t)rt->var_capacity, sizeof(JSVariable));
    rt->func_capacity = 8;
    rt->funcs = (JSFunction*)calloc((size_t)rt->func_capacity, sizeof(JSFunction));

    if (!rt->vars || !rt->funcs) {
        js_destroy_runtime(rt);
        return NULL;
    }

    js_set_string(rt, "BrowserState.version", "1.0.0");
    js_set_string(rt, "navigator.userAgent", "MiniJS/1.0");

    return rt;
}

void js_eval(JSRuntime* rt, const char* script) {
    if (!rt || !script) return;
    printf("[JS Engine] Evaluating script...\n");
    js_execute_script(rt, script, NULL, 0);
}

void js_call_function(JSRuntime* rt, const char* func_name) {
    if (!rt || !func_name) return;

    int idx = js_find_func_index(rt, func_name);
    if (idx < 0) {
        fprintf(stderr, "[JS Engine] Function not found: %s\n", func_name);
        return;
    }

    JSValue ret;
    js_value_make_null(&ret);

    printf("[JS Engine] Calling function: %s()\n", func_name);
    js_execute_script(rt, rt->funcs[idx].body, &ret, 1);

    if (ret.type != JS_VALUE_NULL) {
        char buffer[256];
        js_value_to_string(&ret, buffer, sizeof(buffer));
        printf("[JS Engine] Return value: %s\n", buffer);
    }
}

void js_destroy_runtime(JSRuntime* rt) {
    if (!rt) return;
    if (rt->funcs) {
        for (int i = 0; i < rt->func_count; ++i) {
            free(rt->funcs[i].body);
        }
        free(rt->funcs);
    }
    free(rt->vars);
    free(rt);
}

int js_set_number(JSRuntime* rt, const char* name, double value) {
    if (!rt || !name) return 0;
    JSValue v;
    js_value_make_number(&v, value);
    return js_set_var_internal(rt, name, v, 0);
}

int js_set_string(JSRuntime* rt, const char* name, const char* value) {
    if (!rt || !name) return 0;
    JSValue v;
    js_value_make_string(&v, value ? value : "");
    return js_set_var_internal(rt, name, v, 0);
}

int js_get_number(JSRuntime* rt, const char* name, double* out_value) {
    if (!rt || !name || !out_value) return 0;
    const JSVariable* v = js_find_var(rt, name);
    if (!v) return 0;
    if (v->type == JS_VALUE_NUMBER) {
        *out_value = v->number;
        return 1;
    }
    if (v->type == JS_VALUE_STRING) {
        *out_value = atof(v->string);
        return 1;
    }
    return 0;
}

const char* js_get_string(JSRuntime* rt, const char* name) {
    if (!rt || !name) return NULL;
    const JSVariable* v = js_find_var(rt, name);
    if (!v) return NULL;
    if (v->type == JS_VALUE_STRING) return v->string;
    static char buf[256];
    if (v->type == JS_VALUE_NUMBER) {
        snprintf(buf, sizeof(buf), "%.15g", v->number);
        return buf;
    }
    return EMPTY_STRING;
}

void js_dump_globals(JSRuntime* rt) {
    if (!rt) return;
    printf("=== JS Globals ===\n");
    for (int i = 0; i < rt->var_count; ++i) {
        const JSVariable* v = &rt->vars[i];
        printf("%s = ", v->name);
        if (v->type == JS_VALUE_STRING) printf("\"%s\"", v->string);
        else if (v->type == JS_VALUE_NUMBER) printf("%.15g", v->number);
        else printf("null");
        if (v->is_const) printf(" (const)");
        printf("\n");
    }
    printf("==================\n");
}