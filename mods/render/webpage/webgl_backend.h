#ifndef MLB_WEBGL_BACKEND_H
#define MLB_WEBGL_BACKEND_H
#include <stddef.h>
#include <stdint.h>
typedef struct { int type; double number; const char *string; const void *data; size_t size; int elem_type; } WebGLArg;
typedef struct { int type; double number; char string[2048]; } WebGLResult;
enum { WEBGL_ARG_NUMBER=1, WEBGL_ARG_STRING=2, WEBGL_ARG_BUFFER=3 };
enum { WEBGL_ELEM_U8=1, WEBGL_ELEM_F32=2, WEBGL_ELEM_U16=3, WEBGL_ELEM_U32=4 };
enum { WEBGL_RESULT_NUMBER=1, WEBGL_RESULT_STRING=2 };
int webgl_context_create(int canvas_id, int width, int height);
int webgl_context_resize(int canvas_id, int width, int height);
void webgl_context_destroy(int canvas_id);
int webgl_call(int canvas_id, const char *method, int argc, const WebGLArg *argv, WebGLResult *result);
unsigned char *webgl_readback(int canvas_id, int width, int height, size_t *byte_length);
void webgl_free_buffer(void *p);
int webgl_backend_available(void);
#endif
