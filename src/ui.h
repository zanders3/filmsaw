#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
  uint8_t r, g, b, a;
} Color;
static inline uint32_t color_rgba(Color c) {
  return ((uint32_t)c.r) | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | ((uint32_t)c.a << 24);
}
static inline uint32_t color_rgba_or_default(Color c, Color default_col) {
  uint32_t col = color_rgba(c);
  return col != 0 ? col : color_rgba(default_col);
}
static inline Color color_col(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return (Color){r, g, b, a};
}

typedef struct Rect {
  float minx, miny, maxx, maxy;
} Rect;
Rect rect_cut_left(Rect* rect, float a);
Rect rect_cut_right(Rect* rect, float a);
Rect rect_cut_top(Rect* rect, float a);
Rect rect_cut_bottom(Rect* rect, float a);

Rect rect_inset_left(Rect r, float a);
Rect rect_inset_right(Rect r, float a);
Rect rect_inset_top(Rect r, float a);
Rect rect_inset_bottom(Rect r, float a);

Rect rect_contract(Rect r, float a);
Rect rect_expand(Rect r, float a);
Rect rect_translate(Rect r, float x, float y);

float rect_width(Rect r);
float rect_height(Rect r);
Rect rect_centre(Rect r, float w, float h);
Rect rect_fit(Rect r, float w, float h);

bool rect_contains(const Rect* r, float x, float y);

typedef enum {
  RectCutSide_Left,
  RectCutSide_Right,
  RectCutSide_Top,
  RectCutSide_Bottom,
} RectCutSide;
Rect rect_cut_side(Rect* r, RectCutSide side, float w, float h);

float ui_clamp(float x, float min, float max);
double ui_clampd(double x, double min, double max);

typedef struct FONScontext FONScontext;
typedef struct UI UI;
UI* ui_init(float dpi_scale, FONScontext* font_context, int default_font);
void ui_free(UI* ui);
Rect ui_windowrect(UI* u, float w, float h);

typedef enum {
  UIEvent_None = 0,
  UIEvent_MouseHover = 1,
  UIEvent_MouseDown = 2,
  UIEvent_MouseDrag = 4,
  UIEvent_MouseClick = 8,
  UIEvent_MouseEnter = 16,
  UIEvent_MouseLeave = 32,

  UIEvent_MouseMidDown = 64,
  UIEvent_MouseMidDrag = 128,
} UIEvent;
typedef struct sapp_event sapp_event;
void ui_frame(UI* u);
void ui_handle_event(UI* u, const sapp_event* e);
void ui_skip_ids(UI* u, int count);
UIEvent ui_get_event(UI* u, Rect pos);

typedef struct {
  float x, y;
  UIEvent evt;
  int cur_widget_id;
} Mouse;
Mouse ui_mouse(UI* u, Rect pos);
typedef struct {
  float dx, dy;
} MouseScroll;
MouseScroll ui_mousescroll(UI* u);

void ui_scissor(UI* u, const Rect* scissor);

typedef struct {
  Color bg_color;
  float border_radius, blur_amount;
} BoxStyle;
void ui_draw_box(UI* u, Rect r, BoxStyle* style);

typedef struct sg_image sg_image;
void ui_draw_image(UI* u, Rect r, sg_image img, Rect imgpos);

typedef struct {
  double x, y;
} Vec2;
void ui_draw_lines(UI* u, Color col, float width, Vec2* points, int num_points);

typedef enum {
  TextAlign_None = 0,
  TextAlign_Left = 1,
  TextAlign_Centre = 2,
  TextAlign_Right = 4,
  TextAlign_Top = 8,
  TextAlign_Middle = 16,
  TextAlign_Bottom = 32,
  TextAlign_TopLeft = TextAlign_Left | TextAlign_Top,
} TextAlign;
typedef struct {
  int font;
  float font_size;
  Color col;
  TextAlign align;
} DrawTextOptions;
Rect ui_measure_text(UI* u, Rect* r, RectCutSide side, const char* s, const char* e, const DrawTextOptions* opts);
float ui_measure_text_wh(UI* u, const char* s, const char* e, const DrawTextOptions* opts, float max_width,
                         float* width_out, float* height_out);
void ui_draw_text(UI* u, Rect pos, const char* s, const char* e, const DrawTextOptions* opts);
