#define SOKOL_IMPL
#if defined(_WIN32)
#include <Windows.h>
#define SOKOL_LOG(s)                                                                                                   \
  OutputDebugStringA(s);                                                                                               \
  OutputDebugStringA("\n")
#endif
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_fetch.h"
#include "sokol_glue.h"
#define SOKOL_GL_IMPL
#include "sokol_gl.h"
#define FONTSTASH_IMPLEMENTATION
#include <fontstash/fontstash.h>
#define SOKOL_FONTSTASH_IMPL
#include <sokol/sokol_fontstash.h>

#ifdef _DEBUG
void DebugLogStr(const char* s) {
  OutputDebugStringA(s);
}
#include <stdio.h>

void DebugLog(const char* s, ...) {
  va_list args;
  va_start(args, s);
  char buf[512];
  vsnprintf(buf, sizeof(buf), s, args);
  va_end(args);
  DebugLogStr(buf);
}
#endif
