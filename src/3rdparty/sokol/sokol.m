#define SOKOL_IMPL
#if defined(_WIN32)
#include <Windows.h>
#define SOKOL_LOG(s) OutputDebugStringA(s); OutputDebugStringA("\n")
#endif
#include "../../../builds/resource.h"
/* this is only needed for the debug-inspection headers */
#define SOKOL_TRACE_HOOKS
/* sokol 3D-API defines are provided by build options */
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_time.h"
#include "sokol_fetch.h"
#include "sokol_glue.h"
#include "cimgui/imgui/imconfig.h"
#ifdef IMGUI_ENABLED
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "../cimgui/cimgui.h"
#include "sokol_imgui.h"
#endif

#if defined(_DEBUG) && !defined(__APPLE__)
void DebugLogStr(const char* s) { OutputDebugStringA(s); }
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

#include "../stb/stb_image_write.h"

typedef struct {
    char* buffer;
    int written, cap;
} mem_context;
#ifdef SOKOL_D3D11
static void write_to_mem(void* context, void* data, int size) {
    mem_context* ctx = (mem_context*)context;
    if (ctx->written + size >= ctx->cap) {
        ctx->cap = ctx->cap ? ctx->cap * 2 : (1280 * 1024 * 4);
        ctx->buffer = realloc(ctx->buffer, ctx->cap);
    }
    memcpy(ctx->buffer + ctx->written, data, size);
    ctx->written += size;
}
#endif

sg_image render_grab_gameframe();

int render_grab_screenshot(char** buffer_out, int* len_out) {
    int res = 1;
#ifdef SOKOL_D3D11
    sg_image imgid = render_grab_gameframe();
    const _sg_image_t* img = _sg_lookup_image(&_sg.pools, imgid.id);
    ID3D11Resource* tex = (ID3D11Resource*)img->d3d11.tex2d;
    ID3D11Texture2D* texture_copy = NULL;
    int width = 0, height = 0;
    if (tex) {
        D3D11_TEXTURE2D_DESC description = { 0 };
        ((ID3D11Texture2D*)tex)->lpVtbl->GetDesc((ID3D11Texture2D*)tex, &description);
        width = description.Width;
        height = description.Height;
        description.SampleDesc = (DXGI_SAMPLE_DESC){ .Count = 1 };
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        description.Usage = D3D11_USAGE_STAGING;
        HRESULT h = _sg.d3d11.dev->lpVtbl->CreateTexture2D(_sg.d3d11.dev, &description, NULL, &texture_copy);
        if (h == S_OK) {
            _sg.d3d11.ctx->lpVtbl->CopyResource(_sg.d3d11.ctx, (ID3D11Resource*)texture_copy, tex);
        }
    }
    if (texture_copy) {
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        HRESULT h = _sg.d3d11.ctx->lpVtbl->Map(_sg.d3d11.ctx, (ID3D11Resource*)texture_copy, 0, D3D11_MAP_READ, 0, &mappedResource);
        if (h == S_OK)
        {
            char* pixels = malloc(width * height * 4);
            char* src_line_ptr = mappedResource.pData;
            char* dst_line_ptr = pixels;
            for (int v = 0; v < height; v++) {
                for (int x = 0; x < width; x++) {
                    dst_line_ptr[0] = src_line_ptr[2];
                    dst_line_ptr[1] = src_line_ptr[1];
                    dst_line_ptr[2] = src_line_ptr[0];
                    dst_line_ptr[3] = src_line_ptr[3];
                    dst_line_ptr += 4;
                    src_line_ptr += 4;
                }
                src_line_ptr += (mappedResource.RowPitch - (width * 4));
            }
            mem_context ctx = { 0 };
            res = stbi_write_jpg_to_func(write_to_mem, &ctx, width, height, 4, pixels, 90);
            free(pixels);
            if (res == 0) {
                free(ctx.buffer);
            }
            else {
                *buffer_out = ctx.buffer;
                *len_out = ctx.written;
            }
        }
        _sg.d3d11.ctx->lpVtbl->Unmap(_sg.d3d11.ctx, (ID3D11Resource*)texture_copy, 0);
        texture_copy->lpVtbl->Release(texture_copy);
    }
#endif
    return res;
}
