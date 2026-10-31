#define _GNU_SOURCE
#include "webgl_backend.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifdef _WIN32
int webgl_context_create(int id,int w,int h){(void)id;(void)w;(void)h;return 0;}
int webgl_context_resize(int id,int w,int h){(void)id;(void)w;(void)h;return 0;}
void webgl_context_destroy(int id){(void)id;}
int webgl_call(int id,const char*m,int n,const WebGLArg*a,WebGLResult*r){(void)id;(void)m;(void)n;(void)a;if(r)memset(r,0,sizeof *r);return 0;}
unsigned char* webgl_readback(int id,int w,int h,size_t*n){(void)id;(void)w;(void)h;if(n)*n=0;return NULL;}
void webgl_free_buffer(void*p){free(p);}
int webgl_backend_available(void){return 0;}
#else
/* Minimal EGL/GLES2 ABI declarations keep the browser build independent of development headers.
   Runtime libraries are optional; if EGL or GLES cannot be loaded, getContext('webgl') returns null. */
typedef void *EGLDisplay; typedef void *EGLConfig; typedef void *EGLSurface; typedef void *EGLContext; typedef void *EGLNativeDisplayType;
typedef int EGLint; typedef unsigned int EGLBoolean; typedef unsigned int EGLenum;
typedef unsigned int GLenum; typedef unsigned int GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned char GLboolean; typedef unsigned int GLbitfield; typedef float GLfloat; typedef ptrdiff_t GLsizeiptr; typedef ptrdiff_t GLintptr; typedef unsigned char GLubyte;
#define EGL_FALSE 0
#define EGL_NONE 0x3038
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_DRAW 0x88E0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VALIDATE_STATUS 0x8B83
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_POINTS 0x0000
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_DITHER 0x0BD0
#define GL_LEQUAL 0x0203
#define GL_LESS 0x0201
#define GL_BACK 0x0405
#define GL_FRONT 0x0404
#define GL_CCW 0x0901
#define GL_CW 0x0900
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_ZERO 0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_REPEAT 0x2901
#define GL_RGBA 0x1908
#define GL_RGB 0x1907
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_VERTEX_UNIFORM_VECTORS 0x8DFB
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NO_ERROR 0

typedef EGLDisplay (*PFN_eglGetDisplay)(EGLNativeDisplayType);
typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay,EGLint*,EGLint*);
typedef EGLBoolean (*PFN_eglBindAPI)(EGLenum);
typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay,const EGLint*,EGLConfig*,EGLint,EGLint*);
typedef EGLSurface (*PFN_eglCreatePbufferSurface)(EGLDisplay,EGLConfig,const EGLint*);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay,EGLConfig,EGLContext,const EGLint*);
typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay,EGLSurface,EGLSurface,EGLContext);
typedef EGLBoolean (*PFN_eglDestroySurface)(EGLDisplay,EGLSurface);
typedef EGLBoolean (*PFN_eglDestroyContext)(EGLDisplay,EGLContext);
typedef EGLBoolean (*PFN_eglTerminate)(EGLDisplay);
typedef void *(*PFN_eglGetProcAddress)(const char*);
typedef EGLDisplay (*PFN_eglGetPlatformDisplayEXT)(EGLenum,void*,const EGLint*);

#define GL_FUNCS(X) \
 X(void,glViewport,(GLint,GLint,GLsizei,GLsizei)) X(void,glClearColor,(GLfloat,GLfloat,GLfloat,GLfloat)) X(void,glClear,(GLbitfield)) \
 X(void,glEnable,(GLenum)) X(void,glDisable,(GLenum)) X(void,glScissor,(GLint,GLint,GLsizei,GLsizei)) X(void,glBlendFunc,(GLenum,GLenum)) \
 X(void,glDepthFunc,(GLenum)) X(void,glDepthMask,(GLboolean)) X(void,glClearDepthf,(GLfloat)) X(void,glCullFace,(GLenum)) X(void,glFrontFace,(GLenum)) X(void,glLineWidth,(GLfloat)) X(void,glColorMask,(GLboolean,GLboolean,GLboolean,GLboolean)) \
 X(GLuint,glCreateShader,(GLenum)) X(void,glShaderSource,(GLuint,GLsizei,const char* const*,const GLint*)) X(void,glCompileShader,(GLuint)) X(void,glGetShaderiv,(GLuint,GLenum,GLint*)) X(void,glGetShaderInfoLog,(GLuint,GLsizei,GLsizei*,char*)) X(void,glDeleteShader,(GLuint)) \
 X(GLuint,glCreateProgram,(void)) X(void,glAttachShader,(GLuint,GLuint)) X(void,glDetachShader,(GLuint,GLuint)) X(void,glLinkProgram,(GLuint)) X(void,glValidateProgram,(GLuint)) X(void,glGetProgramiv,(GLuint,GLenum,GLint*)) X(void,glGetProgramInfoLog,(GLuint,GLsizei,GLsizei*,char*)) X(void,glDeleteProgram,(GLuint)) X(void,glUseProgram,(GLuint)) \
 X(GLint,glGetAttribLocation,(GLuint,const char*)) X(GLint,glGetUniformLocation,(GLuint,const char*)) X(void,glGenBuffers,(GLsizei,GLuint*)) X(void,glBindBuffer,(GLenum,GLuint)) X(void,glBufferData,(GLenum,GLsizeiptr,const void*,GLenum)) X(void,glBufferSubData,(GLenum,GLintptr,GLsizeiptr,const void*)) X(void,glDeleteBuffers,(GLsizei,const GLuint*)) \
 X(void,glEnableVertexAttribArray,(GLuint)) X(void,glDisableVertexAttribArray,(GLuint)) X(void,glVertexAttribPointer,(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*)) X(void,glDrawArrays,(GLenum,GLint,GLsizei)) X(void,glDrawElements,(GLenum,GLsizei,GLenum,const void*)) \
 X(void,glUniform1i,(GLint,GLint)) X(void,glUniform2i,(GLint,GLint,GLint)) X(void,glUniform3i,(GLint,GLint,GLint,GLint)) X(void,glUniform4i,(GLint,GLint,GLint,GLint,GLint)) X(void,glUniform1f,(GLint,GLfloat)) X(void,glUniform2f,(GLint,GLfloat,GLfloat)) X(void,glUniform3f,(GLint,GLfloat,GLfloat,GLfloat)) X(void,glUniform4f,(GLint,GLfloat,GLfloat,GLfloat,GLfloat)) X(void,glUniform1fv,(GLint,GLsizei,const GLfloat*)) X(void,glUniform2fv,(GLint,GLsizei,const GLfloat*)) X(void,glUniform3fv,(GLint,GLsizei,const GLfloat*)) X(void,glUniform4fv,(GLint,GLsizei,const GLfloat*)) X(void,glUniform1iv,(GLint,GLsizei,const GLint*)) X(void,glUniform2iv,(GLint,GLsizei,const GLint*)) X(void,glUniform3iv,(GLint,GLsizei,const GLint*)) X(void,glUniform4iv,(GLint,GLsizei,const GLint*)) X(void,glUniformMatrix2fv,(GLint,GLsizei,GLboolean,const GLfloat*)) X(void,glUniformMatrix3fv,(GLint,GLsizei,GLboolean,const GLfloat*)) X(void,glUniformMatrix4fv,(GLint,GLsizei,GLboolean,const GLfloat*)) \
 X(void,glGenTextures,(GLsizei,GLuint*)) X(void,glBindTexture,(GLenum,GLuint)) X(void,glDeleteTextures,(GLsizei,const GLuint*)) X(void,glActiveTexture,(GLenum)) X(void,glTexParameteri,(GLenum,GLenum,GLint)) X(void,glTexImage2D,(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*)) X(void,glGenerateMipmap,(GLenum)) X(void,glPixelStorei,(GLenum,GLint)) \
 X(void,glReadPixels,(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*)) X(GLenum,glGetError,(void)) X(void,glGetIntegerv,(GLenum,GLint*)) X(const GLubyte*,glGetString,(GLenum)) X(void,glFlush,(void)) X(void,glFinish,(void))

typedef struct {
    void *egl_lib, *gles_lib;
    PFN_eglGetDisplay eglGetDisplay; PFN_eglInitialize eglInitialize; PFN_eglBindAPI eglBindAPI;
    PFN_eglChooseConfig eglChooseConfig; PFN_eglCreatePbufferSurface eglCreatePbufferSurface; PFN_eglCreateContext eglCreateContext;
    PFN_eglMakeCurrent eglMakeCurrent; PFN_eglDestroySurface eglDestroySurface; PFN_eglDestroyContext eglDestroyContext; PFN_eglTerminate eglTerminate; PFN_eglGetProcAddress eglGetProcAddress;
    PFN_eglGetPlatformDisplayEXT eglGetPlatformDisplayEXT;
#define DECL(ret,name,args) ret (*name) args;
    GL_FUNCS(DECL)
#undef DECL
    EGLDisplay display; EGLConfig config; int initialized, ok;
} GLApi;
static GLApi G;

typedef struct GLCanvas { int id,w,h; EGLSurface surface; EGLContext context; struct GLCanvas *next; } GLCanvas;
static GLCanvas *contexts;
static void *sym(void *lib,const char *name) { void *p=lib?dlsym(lib,name):NULL; if (!p && G.eglGetProcAddress) p=G.eglGetProcAddress(name); return p; }
static int load_api(void) {
    if (G.ok) return 1; if (G.initialized) return 0; G.initialized=1;
    /* EGL and GLES must come from the same implementation (Mesa, ANGLE, etc.). */
    static const char *pairs[][2] = {
        {"libEGL.so.1", "libGLESv2.so.2"},
        {"libEGL.so", "libGLESv2.so"},
        {"/usr/lib/chromium/libEGL.so", "/usr/lib/chromium/libGLESv2.so"},
        {NULL, NULL}
    };
    for (int i=0; pairs[i][0] && (!G.egl_lib || !G.gles_lib); i++) {
        void *el = dlopen(pairs[i][0], RTLD_NOW|RTLD_LOCAL);
        void *gl = dlopen(pairs[i][1], RTLD_NOW|RTLD_LOCAL);
        if (el && gl) { G.egl_lib=el; G.gles_lib=gl; break; }
        if (el) dlclose(el); if (gl) dlclose(gl);
    }
    if(!G.egl_lib||!G.gles_lib) return 0;
#define LOAD_EGL(n) do { G.n=(void*)dlsym(G.egl_lib,#n); if(!G.n) return 0; } while(0)
    LOAD_EGL(eglGetDisplay); LOAD_EGL(eglInitialize); LOAD_EGL(eglBindAPI); LOAD_EGL(eglChooseConfig); LOAD_EGL(eglCreatePbufferSurface); LOAD_EGL(eglCreateContext); LOAD_EGL(eglMakeCurrent); LOAD_EGL(eglDestroySurface); LOAD_EGL(eglDestroyContext); LOAD_EGL(eglTerminate);
#undef LOAD_EGL
    G.eglGetProcAddress=(PFN_eglGetProcAddress)dlsym(G.egl_lib,"eglGetProcAddress");
    if(G.eglGetProcAddress) G.eglGetPlatformDisplayEXT=(PFN_eglGetPlatformDisplayEXT)G.eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLint maj=0,min=0;
    if(G.eglGetPlatformDisplayEXT) {
        EGLDisplay d=G.eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,NULL,NULL);
        if(d && G.eglInitialize(d,&maj,&min)) G.display=d;
    }
    if(!G.display) {
        EGLDisplay d=G.eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if(d && G.eglInitialize(d,&maj,&min)) G.display=d;
    }
    if(!G.display || !G.eglBindAPI(EGL_OPENGL_ES_API)) return 0;
    EGLint cfg[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,16,EGL_NONE}; EGLint count=0;
    if(!G.eglChooseConfig(G.display,cfg,&G.config,1,&count)||count<1) return 0;
#define LOAD_GL(ret,n,args) do { G.n=(ret (*) args)sym(G.gles_lib,#n); if(!G.n) return 0; } while(0);
    GL_FUNCS(LOAD_GL)
#undef LOAD_GL
    G.ok=1; return 1;
}
int webgl_backend_available(void) { return load_api(); }
static GLCanvas *find_ctx(int id) { for(GLCanvas*c=contexts;c;c=c->next) if(c->id==id)return c; return NULL; }
static int make_current(GLCanvas*c) { return c && G.eglMakeCurrent(G.display,c->surface,c->surface,c->context); }
int webgl_context_create(int id,int w,int h) {
    if(w<1)w=1;if(h<1)h=1;if(w>4096)w=4096;if(h>4096)h=4096;
    if(!load_api())return 0; GLCanvas*old=find_ctx(id); if(old)return webgl_context_resize(id,w,h);
    GLCanvas*c=calloc(1,sizeof *c); if(!c)return 0; c->id=id;c->w=w;c->h=h;
    EGLint pa[]={EGL_WIDTH,w,EGL_HEIGHT,h,EGL_NONE}; c->surface=G.eglCreatePbufferSurface(G.display,G.config,pa);
    EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE}; c->context=G.eglCreateContext(G.display,G.config,NULL,ca);
    if(!c->surface||!c->context||!G.eglMakeCurrent(G.display,c->surface,c->surface,c->context)) { if(c->context)G.eglDestroyContext(G.display,c->context);if(c->surface)G.eglDestroySurface(G.display,c->surface);free(c);return 0; }
    G.glViewport(0,0,w,h); G.glClearColor(0,0,0,0); G.glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); c->next=contexts;contexts=c;return 1;
}
int webgl_context_resize(int id,int w,int h) {
    GLCanvas*c=find_ctx(id);if(!c)return webgl_context_create(id,w,h);if(w<1)w=1;if(h<1)h=1;if(w>4096)w=4096;if(h>4096)h=4096;
    if(c->w==w&&c->h==h)return 1;
    if(!make_current(c))return 0;G.eglDestroySurface(G.display,c->surface);c->surface=NULL;
    EGLint pa[]={EGL_WIDTH,w,EGL_HEIGHT,h,EGL_NONE};c->surface=G.eglCreatePbufferSurface(G.display,G.config,pa);if(!c->surface||!G.eglMakeCurrent(G.display,c->surface,c->surface,c->context))return 0;
    c->w=w;c->h=h;G.glViewport(0,0,w,h);G.glClearColor(0,0,0,0);G.glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);return 1;
}
void webgl_context_destroy(int id) { GLCanvas**pp=&contexts;while(*pp){GLCanvas*c=*pp;if(c->id==id){*pp=c->next;if(G.ok){G.eglMakeCurrent(G.display,NULL,NULL,NULL);if(c->surface)G.eglDestroySurface(G.display,c->surface);if(c->context)G.eglDestroyContext(G.display,c->context);}free(c);return;}pp=&c->next;} }
static double num(const WebGLArg*a,int i,double def){return i>=0&&i<16&&a[i].type==WEBGL_ARG_NUMBER?a[i].number:def;}
static const char *str(const WebGLArg*a,int i){return i>=0&&i<16&&a[i].type==WEBGL_ARG_STRING&&a[i].string?a[i].string:"";}
static void ret_num(WebGLResult*r,double n){if(r){memset(r,0,sizeof *r);r->type=WEBGL_RESULT_NUMBER;r->number=n;}}
static void ret_str(WebGLResult*r,const char*s){if(r){memset(r,0,sizeof *r);r->type=WEBGL_RESULT_STRING;snprintf(r->string,sizeof r->string,"%s",s?s:"");}}
#define A(i) ((int)num(argv,i,0))
#define F(i) ((GLfloat)num(argv,i,0))
int webgl_call(int id,const char *m,int argc,const WebGLArg *argv,WebGLResult*r) {
    GLCanvas*c=find_ctx(id);if(!c||!m||!load_api()||!make_current(c))return 0;ret_num(r,0);
    if(!strcmp(m,"resize"))return webgl_context_resize(id,A(0),A(1));
    if(!strcmp(m,"viewport")){G.glViewport(A(0),A(1),A(2),A(3));return 1;}
    if(!strcmp(m,"clearColor")){G.glClearColor(F(0),F(1),F(2),F(3));return 1;}
    if(!strcmp(m,"clear")){G.glClear((GLbitfield)A(0));return 1;}
    if(!strcmp(m,"enable")){G.glEnable((GLenum)A(0));return 1;} if(!strcmp(m,"disable")){G.glDisable((GLenum)A(0));return 1;}
    if(!strcmp(m,"scissor")){G.glScissor(A(0),A(1),A(2),A(3));return 1;} if(!strcmp(m,"blendFunc")){G.glBlendFunc((GLenum)A(0),(GLenum)A(1));return 1;}
    if(!strcmp(m,"depthFunc")){G.glDepthFunc((GLenum)A(0));return 1;} if(!strcmp(m,"depthMask")){G.glDepthMask((GLboolean)!!A(0));return 1;} if(!strcmp(m,"clearDepth")){G.glClearDepthf(F(0));return 1;}
    if(!strcmp(m,"cullFace")){G.glCullFace((GLenum)A(0));return 1;} if(!strcmp(m,"frontFace")){G.glFrontFace((GLenum)A(0));return 1;} if(!strcmp(m,"lineWidth")){G.glLineWidth(F(0));return 1;}
    if(!strcmp(m,"colorMask")){G.glColorMask(A(0)!=0,A(1)!=0,A(2)!=0,A(3)!=0);return 1;}
    if(!strcmp(m,"createShader")){ret_num(r,G.glCreateShader((GLenum)A(0)));return 1;}
    if(!strcmp(m,"shaderSource")){GLuint sh=(GLuint)A(0);const char*s=str(argv,1);G.glShaderSource(sh,1,&s,NULL);return 1;}
    if(!strcmp(m,"compileShader")){G.glCompileShader((GLuint)A(0));return 1;}
    if(!strcmp(m,"getShaderParameter")){GLint v=0;G.glGetShaderiv((GLuint)A(0),(GLenum)A(1),&v);ret_num(r,v);return 1;}
    if(!strcmp(m,"getShaderInfoLog")){char b[2048]={0};GLsizei n=0;G.glGetShaderInfoLog((GLuint)A(0),sizeof b-1,&n,b);b[n>=0&&n<(int)sizeof b?n:(int)sizeof b-1]=0;ret_str(r,b);return 1;}
    if(!strcmp(m,"deleteShader")){G.glDeleteShader((GLuint)A(0));return 1;}
    if(!strcmp(m,"createProgram")){ret_num(r,G.glCreateProgram());return 1;} if(!strcmp(m,"attachShader")){G.glAttachShader((GLuint)A(0),(GLuint)A(1));return 1;}
    if(!strcmp(m,"detachShader")){G.glDetachShader((GLuint)A(0),(GLuint)A(1));return 1;} if(!strcmp(m,"linkProgram")){G.glLinkProgram((GLuint)A(0));return 1;}
    if(!strcmp(m,"validateProgram")){G.glValidateProgram((GLuint)A(0));return 1;}
    if(!strcmp(m,"getProgramParameter")){GLint v=0;G.glGetProgramiv((GLuint)A(0),(GLenum)A(1),&v);ret_num(r,v);return 1;}
    if(!strcmp(m,"getProgramInfoLog")){char b[2048]={0};GLsizei n=0;G.glGetProgramInfoLog((GLuint)A(0),sizeof b-1,&n,b);b[n>=0&&n<(int)sizeof b?n:(int)sizeof b-1]=0;ret_str(r,b);return 1;}
    if(!strcmp(m,"deleteProgram")){G.glDeleteProgram((GLuint)A(0));return 1;} if(!strcmp(m,"useProgram")){G.glUseProgram((GLuint)A(0));return 1;}
    if(!strcmp(m,"getAttribLocation")){ret_num(r,G.glGetAttribLocation((GLuint)A(0),str(argv,1)));return 1;} if(!strcmp(m,"getUniformLocation")){ret_num(r,G.glGetUniformLocation((GLuint)A(0),str(argv,1)));return 1;}
    if(!strcmp(m,"createBuffer")){GLuint h=0;G.glGenBuffers(1,&h);ret_num(r,h);return 1;} if(!strcmp(m,"bindBuffer")){G.glBindBuffer((GLenum)A(0),(GLuint)A(1));return 1;}
    if(!strcmp(m,"bufferData")){if(argc<3)return 0;const void *p=argv[1].type==WEBGL_ARG_BUFFER?argv[1].data:NULL;size_t n=argv[1].type==WEBGL_ARG_BUFFER?argv[1].size:(size_t)A(1);if(n>(size_t)512*1024*1024)return 0;G.glBufferData((GLenum)A(0),(GLsizeiptr)n,p,(GLenum)A(2));return 1;}
    if(!strcmp(m,"bufferSubData")){if(argc<3||argv[2].type!=WEBGL_ARG_BUFFER||argv[2].size>(size_t)512*1024*1024)return 0;G.glBufferSubData((GLenum)A(0),(GLintptr)A(1),(GLsizeiptr)argv[2].size,argv[2].data);return 1;}
    if(!strcmp(m,"deleteBuffer")){GLuint v=(GLuint)A(0);G.glDeleteBuffers(1,&v);return 1;} if(!strcmp(m,"enableVertexAttribArray")){G.glEnableVertexAttribArray((GLuint)A(0));return 1;}
    if(!strcmp(m,"disableVertexAttribArray")){G.glDisableVertexAttribArray((GLuint)A(0));return 1;}
    if(!strcmp(m,"vertexAttribPointer")){uintptr_t off=(uintptr_t)num(argv,5,0);G.glVertexAttribPointer((GLuint)A(0),A(1),(GLenum)A(2),A(3)!=0,A(4),(const void*)off);return 1;}
    if(!strcmp(m,"drawArrays")){G.glDrawArrays((GLenum)A(0),A(1),A(2));return 1;} if(!strcmp(m,"drawElements")){uintptr_t off=(uintptr_t)num(argv,3,0);G.glDrawElements((GLenum)A(0),A(1),(GLenum)A(2),(const void*)off);return 1;}
    if(!strcmp(m,"uniform1i")){G.glUniform1i(A(0),A(1));return 1;} if(!strcmp(m,"uniform2i")){G.glUniform2i(A(0),A(1),A(2));return 1;} if(!strcmp(m,"uniform3i")){G.glUniform3i(A(0),A(1),A(2),A(3));return 1;} if(!strcmp(m,"uniform4i")){G.glUniform4i(A(0),A(1),A(2),A(3),A(4));return 1;} if(!strcmp(m,"uniform1f")){G.glUniform1f(A(0),F(1));return 1;}
    if(!strcmp(m,"uniform2f")){G.glUniform2f(A(0),F(1),F(2));return 1;} if(!strcmp(m,"uniform3f")){G.glUniform3f(A(0),F(1),F(2),F(3));return 1;} if(!strcmp(m,"uniform4f")){G.glUniform4f(A(0),F(1),F(2),F(3),F(4));return 1;}
    if(!strcmp(m,"uniform1fv")||!strcmp(m,"uniform2fv")||!strcmp(m,"uniform3fv")||!strcmp(m,"uniform4fv")){const GLfloat*p=argc>1&&argv[1].type==WEBGL_ARG_BUFFER?(const GLfloat*)argv[1].data:NULL;if(!p)return 0;GLsizei n=(GLsizei)(argv[1].size/(sizeof(GLfloat)*(m[7]-'0')));if(m[7]=='1')G.glUniform1fv(A(0),n,p);else if(m[7]=='2')G.glUniform2fv(A(0),n,p);else if(m[7]=='3')G.glUniform3fv(A(0),n,p);else G.glUniform4fv(A(0),n,p);return 1;}
    if(!strcmp(m,"uniform1iv")||!strcmp(m,"uniform2iv")||!strcmp(m,"uniform3iv")||!strcmp(m,"uniform4iv")){const GLint*p=argc>1&&argv[1].type==WEBGL_ARG_BUFFER?(const GLint*)argv[1].data:NULL;if(!p)return 0;GLsizei n=(GLsizei)(argv[1].size/(sizeof(GLint)*(m[7]-'0')));if(m[7]=='1')G.glUniform1iv(A(0),n,p);else if(m[7]=='2')G.glUniform2iv(A(0),n,p);else if(m[7]=='3')G.glUniform3iv(A(0),n,p);else G.glUniform4iv(A(0),n,p);return 1;}
    if(!strcmp(m,"uniformMatrix2fv")||!strcmp(m,"uniformMatrix3fv")||!strcmp(m,"uniformMatrix4fv")){const GLfloat*p=argc>2&&argv[2].type==WEBGL_ARG_BUFFER?(const GLfloat*)argv[2].data:NULL;if(!p)return 0;if(m[13]=='2')G.glUniformMatrix2fv(A(0),1,A(1)!=0,p);else if(m[13]=='3')G.glUniformMatrix3fv(A(0),1,A(1)!=0,p);else G.glUniformMatrix4fv(A(0),1,A(1)!=0,p);return 1;}
    if(!strcmp(m,"createTexture")){GLuint h=0;G.glGenTextures(1,&h);ret_num(r,h);return 1;} if(!strcmp(m,"bindTexture")){G.glBindTexture((GLenum)A(0),(GLuint)A(1));return 1;}
    if(!strcmp(m,"deleteTexture")){GLuint h=(GLuint)A(0);G.glDeleteTextures(1,&h);return 1;} if(!strcmp(m,"activeTexture")){G.glActiveTexture((GLenum)A(0));return 1;}
    if(!strcmp(m,"texParameteri")){G.glTexParameteri((GLenum)A(0),(GLenum)A(1),A(2));return 1;}
    if(!strcmp(m,"generateMipmap")){G.glGenerateMipmap((GLenum)A(0));return 1;}
    if(!strcmp(m,"texImage2D")){if(argc<9)return 0;const void*p=argc>8&&argv[8].type==WEBGL_ARG_BUFFER?argv[8].data:NULL;G.glTexImage2D((GLenum)A(0),A(1),A(2),A(3),A(4),A(5),(GLenum)A(6),(GLenum)A(7),p);return 1;}
    if(!strcmp(m,"pixelStorei")){G.glPixelStorei((GLenum)A(0),A(1));return 1;}
    if(!strcmp(m,"getParameter")){GLint v=0;switch(A(0)){case GL_MAX_TEXTURE_SIZE:case GL_MAX_VERTEX_ATTRIBS:case GL_MAX_TEXTURE_IMAGE_UNITS:case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:case GL_MAX_VERTEX_UNIFORM_VECTORS:case GL_MAX_FRAGMENT_UNIFORM_VECTORS:G.glGetIntegerv((GLenum)A(0),&v);ret_num(r,v);return 1;case GL_VERSION:case GL_VENDOR:case GL_RENDERER:case GL_SHADING_LANGUAGE_VERSION:{const GLubyte*s=G.glGetString((GLenum)A(0));ret_str(r,(const char*)s);return 1;}default:ret_num(r,0);return 1;}}
    if(!strcmp(m,"getError")){ret_num(r,G.glGetError());return 1;} if(!strcmp(m,"flush")){G.glFlush();return 1;} if(!strcmp(m,"finish")){G.glFinish();return 1;}
    if(!strcmp(m,"isBuffer")||!strcmp(m,"isTexture")||!strcmp(m,"isProgram")||!strcmp(m,"isShader")){ret_num(r,A(0)!=0);return 1;}
    (void)argc;return 0;
}
unsigned char *webgl_readback(int id,int w,int h,size_t *byte_length) {
    GLCanvas*c=find_ctx(id);if(!c||!make_current(c)||w<1||h<1||w>4096||h>4096)return NULL;
    if(w>c->w)w=c->w;if(h>c->h)h=c->h;size_t n=(size_t)w*h*4;if(n>256u*1024u*1024u)return NULL;
    unsigned char*raw=malloc(n),*out=malloc(n);if(!raw||!out){free(raw);free(out);return NULL;}
    G.glPixelStorei(GL_PACK_ALIGNMENT,1);G.glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,raw);
    size_t row=(size_t)w*4;for(int y=0;y<h;y++)memcpy(out+(size_t)y*row,raw+(size_t)(h-1-y)*row,row);free(raw);if(byte_length)*byte_length=n;return out;
}
void webgl_free_buffer(void*p){free(p);}
#endif
