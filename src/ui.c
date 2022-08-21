#include "ui.h"
#include <stdlib.h>
#include <math.h>
#include <fontstash/fontstash.h>
#include <sokol/sokol_gfx.h>
#include <sokol/sokol_gl.h>
#include <sokol/sokol_app.h>
#include "../data/box.h"

Rect rect_cut_left(Rect* rect, float a) {
  float minx = rect->minx;
  rect->minx = (float)fmin(rect->maxx, rect->minx + a);
  return (Rect){minx, rect->miny, rect->minx, rect->maxy};
}
Rect rect_cut_right(Rect* rect, float a) {
  float maxx = rect->maxx;
  rect->maxx = (float)fmax(rect->minx, rect->maxx - a);
  return (Rect){rect->maxx, rect->miny, maxx, rect->maxy};
}
Rect rect_cut_top(Rect* rect, float a) {
  float miny = rect->miny;
  rect->miny = (float)fmin(rect->maxy, rect->miny + a);
  return (Rect){rect->minx, miny, rect->maxx, rect->miny};
}
Rect rect_cut_bottom(Rect* rect, float a) {
  float maxy = rect->maxy;
  rect->maxy = (float)fmax(rect->miny, rect->maxy - a);
  return (Rect){rect->minx, rect->maxy, rect->maxx, maxy};
}
Rect rect_inset_left(Rect rect, float a) {
  float minx = rect.minx;
  rect.minx = (float)fmin(rect.maxx, rect.minx + a);
  return (Rect){minx, rect.miny, rect.minx, rect.maxy};
}
Rect rect_inset_right(Rect rect, float a) {
  float maxx = rect.maxx;
  rect.maxx = (float)fmax(rect.minx, rect.maxx - a);
  return (Rect){rect.maxx, rect.miny, maxx, rect.maxy};
}
Rect rect_inset_top(Rect rect, float a) {
  float miny = rect.miny;
  rect.miny = (float)fmin(rect.maxy, rect.miny + a);
  return (Rect){rect.minx, miny, rect.maxx, rect.miny};
}
Rect rect_inset_bottom(Rect rect, float a) {
  float maxy = rect.maxy;
  rect.maxy = (float)fmax(rect.miny, rect.maxy - a);
  return (Rect){rect.minx, rect.maxy, rect.maxx, maxy};
}
Rect rect_contract(Rect r, float a) {
  return (Rect){r.minx + a, r.miny + a, r.maxx - a, r.maxy - a};
}
Rect rect_expand(Rect r, float a) {
  return (Rect){r.minx - a, r.miny - a, r.maxx + a, r.maxy + a};
}
bool rect_contains(const Rect* r, float x, float y) {
  return x >= r->minx && x <= r->maxx && y >= r->miny && y <= r->maxy;
}
Rect rect_translate(Rect r, float x, float y) {
  return (Rect){r.minx + x, r.miny + y, r.maxx + x, r.maxy + y};
}
float rect_height(Rect r) {
  return r.maxy - r.miny;
}
float rect_width(Rect r) {
  return r.maxx - r.minx;
}
Rect rect_centre(Rect r, float w, float h) {
  float rw = r.maxx - r.minx, rh = r.maxy - r.miny;
  float mx = r.minx + (rw - w) * 0.5f, my = r.miny + (rh - h) * 0.5f;
  return (Rect){mx, my, mx + w, my + h};
}
Rect rect_fit(Rect r, float w, float h) {
  float rw = r.maxx - r.minx, rh = r.maxy - r.miny;
  float th = h * rw / w;
  float tw = w * rh / h;
  float scale = th > h ? tw / w : th / h;
  return rect_centre(r, w * scale, h * scale);
}
Rect rect_cut_side(Rect* r, RectCutSide side, float w, float h) {
  switch (side) {
  default:
  case RectCutSide_Left:
    return rect_cut_left(r, w);
  case RectCutSide_Top:
    return rect_cut_top(r, h);
  case RectCutSide_Right:
    return rect_cut_right(r, w);
  case RectCutSide_Bottom:
    return rect_cut_bottom(r, h);
  }
}

float ui_clamp(float x, float min, float max) {
  return x < min ? min : x > max ? max : x;
}
double ui_clampd(double x, double min, double max) {
  return x < min ? min : x > max ? max : x;
}

typedef struct UI {
  float dpi_scale;
  float inv_dpi_scale;
  Rect scissor;
  int default_font;
  FONScontext* font_ctx;
  sgl_pipeline box_pip, box_flat_pip;

  float mousex, mousey, prevmousescrollx, prevmousescrolly, mousescrollx, mousescrolly;
  UIEvent evts;
  bool mouseclick;

  int widget_id, cur_widget_id, next_widget_id;
  int entered_widget_id, left_widget_id;
} UI;
UI* ui_init(float dpi_scale, FONScontext* font_context, int default_font) {
  UI* u = (UI*)malloc(sizeof(UI));
  *u = (UI){.dpi_scale = dpi_scale,
            .inv_dpi_scale = 1.0f / dpi_scale,
            .font_ctx = font_context,
            .default_font = default_font,
            .evts = UIEvent_MouseHover,
            .widget_id = 1};
  sg_shader shader = sg_make_shader(box_shader_desc(sg_query_backend()));
  u->box_pip = sgl_make_pipeline(&(sg_pipeline_desc){
      .shader = shader,
      .colors[0].blend = {.enabled = true,
                          .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                          .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA},
  });
  u->box_flat_pip = sgl_make_pipeline(&(sg_pipeline_desc){
      .colors[0].blend = {.enabled = true,
                          .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                          .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA},
  });
  return u;
}
void ui_free(UI* ui) {
  free(ui);
}
Rect ui_windowrect(UI* u, float w, float h) {
  return (Rect){.maxx = w * u->inv_dpi_scale, .maxy = h * u->inv_dpi_scale};
}

void ui_skip_ids(UI* u, int count) {
  u->widget_id += count;
}
UIEvent ui_get_event(UI* u, Rect pos) {
  int widget_id = u->widget_id++;
  if (u->evts & (UIEvent_MouseDown | UIEvent_MouseMidDown)) {
    if (widget_id == u->cur_widget_id) {
      u->next_widget_id = u->cur_widget_id;
      return u->evts;
    }
    return UIEvent_None;
  }
  if (rect_contains(&pos, u->mousex, u->mousey)) {
    if (u->next_widget_id < widget_id) {
      u->next_widget_id = widget_id;
    }
    if (u->cur_widget_id == widget_id) {
      int evts = u->evts;
      if (u->entered_widget_id == widget_id) {
        u->entered_widget_id = 0;
        evts |= UIEvent_MouseEnter;
      }
      return evts;
    }
  }
  if (u->left_widget_id == widget_id) {
    u->left_widget_id = 0;
    return UIEvent_MouseLeave;
  }
  return UIEvent_None;
}
void ui_frame(UI* u) {
  u->prevmousescrollx = u->mousescrollx;
  u->prevmousescrolly = u->mousescrolly;
  u->mousescrollx = 0.0f;
  u->mousescrolly = 0.0f;
  u->widget_id = 1;
  if (u->cur_widget_id != u->next_widget_id) {
    u->entered_widget_id = u->next_widget_id;
    if (u->cur_widget_id != 0) {
      u->left_widget_id = u->cur_widget_id;
    }
    u->cur_widget_id = u->next_widget_id;
  }
  u->next_widget_id = 0;
  if (u->mouseclick) {
    u->mouseclick = false;
    u->evts &= ~UIEvent_MouseClick;
  } else if (u->evts & UIEvent_MouseClick) { // delay removal of mouseclick by 1 frame
    u->mouseclick = true;
  }
}
void ui_handle_event(UI* u, const sapp_event* e) {
  switch (e->type) {
  default:
    break;
  case SAPP_EVENTTYPE_MOUSE_DOWN:
    if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
      u->evts |= UIEvent_MouseDown | UIEvent_MouseClick;
    } else if (e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) {
      u->evts |= UIEvent_MouseMidDown;
    }
    break;
  case SAPP_EVENTTYPE_MOUSE_MOVE:
    if (u->evts & UIEvent_MouseDown) {
      u->evts |= UIEvent_MouseDrag;
    }
    if (u->evts & UIEvent_MouseMidDown) {
      u->evts |= UIEvent_MouseMidDrag;
    }
    break;
  case SAPP_EVENTTYPE_MOUSE_UP:
    if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
      u->evts &= ~(UIEvent_MouseClick | UIEvent_MouseDown | UIEvent_MouseDrag);
    } else if (e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) {
      u->evts &= ~(UIEvent_MouseMidDown | UIEvent_MouseMidDrag);
    }
    break;
  case SAPP_EVENTTYPE_MOUSE_ENTER:
    u->evts |= UIEvent_MouseHover;
    break;
  case SAPP_EVENTTYPE_MOUSE_LEAVE:
    u->evts &= ~UIEvent_MouseHover;
    break;
  }
  u->mousex = e->mouse_x * u->inv_dpi_scale;
  u->mousey = e->mouse_y * u->inv_dpi_scale;
  if (e->type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
    u->mousescrollx += e->scroll_x * 10.0f;
    u->mousescrolly += e->scroll_y * 10.0f;
  }
}
Mouse ui_mouse(UI* u, Rect pos) {
  return (Mouse){
      .evt = u->evts,
      .cur_widget_id = u->cur_widget_id,
      .x = u->mousex - pos.minx,
      .y = u->mousey - pos.miny,
  };
}
MouseScroll ui_mousescroll(UI* u) {
  return (MouseScroll){u->prevmousescrollx, u->prevmousescrolly};
}
void ui_scissor(UI* u, const Rect* scissor) {
  Rect s = scissor ? *scissor : (Rect){0.0f, 0.0f, sapp_widthf(), sapp_heightf()};
  const float dpis = u->dpi_scale;
  s.minx *= dpis;
  s.miny *= dpis;
  s.maxx *= dpis;
  s.maxy *= dpis;
  u->scissor = s;
  sgl_scissor_rectf(s.minx, s.miny, s.maxx - s.minx, s.maxy - s.miny, true);
}
float ui_measure_text_wh(UI* u, const char* s, const char* e, const DrawTextOptions* opts, float max_width,
                         float* width_out, float* height_out) {
  const float dpis = u->dpi_scale;
  fonsSetAlign(u->font_ctx, FONS_ALIGN_TOP | FONS_ALIGN_LEFT);
  fonsSetFont(u->font_ctx, opts && opts->font ? opts->font : u->default_font);
  fonsSetSize(u->font_ctx, opts && opts->font_size ? opts->font_size * dpis : 24.0f * dpis);
  float lh;
  fonsVertMetrics(u->font_ctx, 0, 0, &lh);
  lh *= u->inv_dpi_scale;
  float total_width = 0.0f;
  float total_height = 0.0f;
  while (s != e && *s != '\0') {
    const char* lineend = s;
    float linewidth = 0.0f;
    while (lineend != e && *lineend && *lineend != '\n') {
      const char* wordend = lineend;
      while (wordend != e && *wordend && *wordend != ' ' && *wordend != '\n') {
        wordend++;
      }
      while (wordend != e && *wordend && *wordend == ' ') {
        wordend++;
      }
      float newlinewidth = fonsTextBounds(u->font_ctx, 0.0f, 0.0f, s, wordend, 0) * u->inv_dpi_scale;
      if (lineend != s && *lineend && max_width > 0.0f && newlinewidth > max_width) {
        break; // went out of bounds - split at previous word. not going to bother splitting long words.
      }
      lineend = wordend;
      linewidth = newlinewidth;
    }
    total_height += lh;
    s = lineend;
    while (s != e && *s && (*s == '\n' || *s == ' ')) {
      s++;
    }
    if (linewidth > total_width) {
      total_width = linewidth;
    }
  }
  if (s != e && e && *(e - 1) == ' ') {
    total_width += fonsTextBounds(u->font_ctx, 0.0f, 0.0f, "  ", 0, 0) * u->inv_dpi_scale;
  }
  if (width_out) {
    *width_out = total_width;
  }
  if (height_out) {
    *height_out = total_height;
  }
  return total_width;
}

Color text_color = {231, 231, 231, 255};

Rect ui_measure_text(UI* u, Rect* r, RectCutSide side, const char* s, const char* e, const DrawTextOptions* opts) {
  float total_width = 0.0f, total_height = 0.0f;
  ui_measure_text_wh(u, s, e, opts, r->maxx - r->minx, &total_width, &total_height);
  return rect_cut_side(r, side, total_width, total_height);
}
void ui_draw_text(UI* u, Rect pos, const char* s, const char* e, const DrawTextOptions* opts) {
  const float total_width = pos.maxx - pos.minx, total_height = pos.maxy - pos.miny;
  const float dpis = u->dpi_scale;
  fonsSetFont(u->font_ctx, opts && opts->font ? opts->font : u->default_font);
  fonsSetSize(u->font_ctx, opts && opts->font_size ? opts->font_size * dpis : 24.0f * dpis);
  float lh, asc;
  fonsVertMetrics(u->font_ctx, &asc, 0, &lh);
  fonsSetAlign(u->font_ctx, FONS_ALIGN_TOP | FONS_ALIGN_LEFT);
  lh *= u->inv_dpi_scale;
  TextAlign align = opts && opts->align ? opts->align : TextAlign_TopLeft;
  uint32_t col = color_rgba_or_default(opts ? opts->col : text_color, text_color);
  fonsSetColor(u->font_ctx, col);
  if (align & TextAlign_Centre) {
    pos.minx = pos.minx + total_width * 0.5f;
  } else if (align & TextAlign_Right) {
    pos.minx = pos.maxx - total_width;
  }
  if (align & TextAlign_Middle) {
    asc *= u->inv_dpi_scale;
    pos.miny = pos.miny + total_height * 0.5f - asc;
  } else if (align & TextAlign_Bottom) {
    pos.miny = pos.maxy - lh;
  }
  while (s != e && *s != '\0') {
    const char* lineend = s;
    float linewidth = 0.0f;
    while (lineend != e && *lineend && *lineend != '\n') {
      const char* wordend = lineend;
      while (wordend != e && *wordend && *wordend != ' ' && *wordend != '\n') {
        wordend++;
      }
      while (wordend != e && *wordend && *wordend == ' ') {
        wordend++;
      }
      float newlinewidth = fonsTextBounds(u->font_ctx, 0.0f, 0.0f, s, wordend, 0);
      newlinewidth *= u->inv_dpi_scale;
      if (lineend != s && *lineend && newlinewidth > total_width) {
        break; // went out of bounds - split at previous word. not going to bother splitting long words.
      }
      lineend = wordend;
      linewidth = newlinewidth;
    }
    float x = pos.minx, y = pos.miny;
    if (align & TextAlign_Right) {
      x = pos.maxx - linewidth;
    } else if (align & TextAlign_Centre) {
      x = pos.minx + linewidth * 0.5f;
    }
    fonsDrawText(u->font_ctx, x * dpis, y * dpis, s, lineend);
    pos.miny += lh;
    s = lineend;
    while (s != e && *s && (*s == '\n' || *s == ' ')) {
      s++;
    }
  }
}
void ui_draw_box(UI* u, Rect r, BoxStyle* style) {
  sgl_disable_texture();
  sgl_begin_quads();
  sgl_load_pipeline(style->blur_amount == 0.0f && style->border_radius == 0.0f ? u->box_flat_pip : u->box_pip);
  uint32_t col = color_rgba(style->bg_color);
  sgl_c1i(col);

  if (r.maxx < r.minx) {
    float t = r.maxx;
    r.maxx = r.minx;
    r.minx = t;
  }
  if (r.maxy < r.miny) {
    float t = r.maxy;
    r.maxy = r.miny;
    r.miny = t;
  }

  const float dpis = u->dpi_scale;
  r.minx *= dpis;
  r.miny *= dpis;
  r.maxx *= dpis;
  r.maxy *= dpis;
  float aspect = (r.maxx - r.minx) / (r.maxy - r.miny);
  float w = aspect, h = 1.0f;
  sgl_frag_size(aspect, 1.0f);
  float rd = (fmaxf(r.maxx - r.minx, r.maxy - r.miny) * 2.0f) / dpis;
  sgl_point_size(style->border_radius * (30.0f / rd), style->blur_amount);
  if (aspect > 1.0f) {
    w = 1.0f;
    h = 1.0f / aspect;
    sgl_frag_size(1.0f, 1.0f / aspect);
  }
  sgl_v2f_t2f(r.minx, r.miny, -w, -h);
  sgl_v2f_t2f(r.maxx, r.miny, w, -h);
  sgl_v2f_t2f(r.maxx, r.maxy, w, h);
  sgl_v2f_t2f(r.minx, r.maxy, -w, h);
  sgl_end();
}

void ui_draw_image(UI* u, Rect r, sg_image img, Rect imgpos) {
  sgl_enable_texture();
  sgl_texture(img);
  sgl_begin_quads();
  sgl_load_pipeline(u->box_flat_pip);
  sgl_c1i(0xFFFFFFFF);
  sgl_v2f_t2f(r.minx, r.miny, imgpos.minx, imgpos.miny);
  sgl_v2f_t2f(r.maxx, r.miny, imgpos.maxx, imgpos.miny);
  sgl_v2f_t2f(r.maxx, r.maxy, imgpos.maxx, imgpos.maxy);
  sgl_v2f_t2f(r.minx, r.maxy, imgpos.minx, imgpos.maxy);
  sgl_end();
}

static double vec2_len(Vec2 v) {
  return sqrt(v.x * v.x + v.y * v.y);
}
static void vec2_norm(Vec2* v) {
  double len = vec2_len(*v);
  if (len == 0.0) {
    len = 1.0;
  }
  v->x /= len;
  v->y /= len;
}
static Vec2 vec2_sub(Vec2 a, Vec2 b) {
  return (Vec2){.x = a.x - b.y, .y = a.y - b.y};
}
static Vec2 vec2_add(Vec2 a, Vec2 b) {
  return (Vec2){.x = a.x + b.y, .y = a.y + b.y};
}

void ui_draw_lines(UI* u, Color col, float width, Vec2* points, int num_points) {
  sgl_disable_texture();
  sgl_begin_quads();
  sgl_load_pipeline(u->box_flat_pip);
  for (int i = 1; i < num_points; i++) {
    Vec2 start = points[i - 1];
    Vec2 end = points[i];
    Vec2 dir = (Vec2){.x = end.x - start.x, .y = end.y - start.y};
    vec2_norm(&dir);
    dir = (Vec2){.x = -dir.y * width, .y = dir.x * width};
    sgl_v2f_c4b((float)(start.x + dir.x), (float)(start.y + dir.y), col.r, col.g, col.b, 0);
    sgl_v2f_c4b((float)(end.x + dir.x), (float)(end.y + dir.y), col.r, col.g, col.b, 0);
    sgl_v2f_c4b((float)end.x, (float)end.y, col.r, col.g, col.b, 255);
    sgl_v2f_c4b((float)start.x, (float)start.y, col.r, col.g, col.b, 255);

    sgl_v2f_c4b((float)(start.x - dir.x), (float)(start.y - dir.y), col.r, col.g, col.b, 0);
    sgl_v2f_c4b((float)(end.x - dir.x), (float)(end.y - dir.y), col.r, col.g, col.b, 0);
    sgl_v2f_c4b((float)end.x, (float)end.y, col.r, col.g, col.b, 255);
    sgl_v2f_c4b((float)start.x, (float)start.y, col.r, col.g, col.b, 255);
  }
  sgl_end();
}
