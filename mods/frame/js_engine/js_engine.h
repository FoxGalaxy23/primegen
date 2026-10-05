#ifndef JS_ENGINE_H
#define JS_ENGINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JS_VALUE_NULL = 0,
    JS_VALUE_NUMBER,
    JS_VALUE_STRING
} JSValueType;

typedef struct {
    char name[64];
    JSValueType type;
    double number;
    char string[2048];
    int is_const;
} JSVariable;

typedef struct JSFunction JSFunction;

typedef struct {
    JSVariable* vars;
    int var_count;
    int var_capacity;

    JSFunction* funcs;
    int func_count;
    int func_capacity;

    void (*dom_set_value_cb)(void* user_data, const char* path, const char* value);
    void* user_data;
} JSRuntime;

JSRuntime* js_create_runtime(void);
void js_set_dom_callback(JSRuntime* rt, void (*cb)(void*, const char*, const char*), void* user_data);
void js_eval(JSRuntime* rt, const char* script);
void js_call_function(JSRuntime* rt, const char* func_name);
void js_destroy_runtime(JSRuntime* rt);
int js_set_number(JSRuntime* rt, const char* name, double value);
int js_set_string(JSRuntime* rt, const char* name, const char* value);
int js_get_number(JSRuntime* rt, const char* name, double* out_value);
const char* js_get_string(JSRuntime* rt, const char* name);
void js_dump_globals(JSRuntime* rt);

#ifdef __cplusplus
}
#endif

#endif
