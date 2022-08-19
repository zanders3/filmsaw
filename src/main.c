#include <sokol/sokol_app.h>
#include <sokol/sokol_gfx.h>
#include <sokol/sokol_glue.h>
#include <stdio.h>
#include "debuglog.h"
#include <sokol/sokol_gl.h>
#include <fontstash/fontstash.h>
#include <sokol/sokol_fontstash.h>
#include <stb/stb_image.h>
#include "ui.h"
#include "video.h"
#include <assert.h>
#include <math.h>
#include <memory.h>
#include <dirent.h>
#include "video_clips.h"

enum IconType {
  IconType_Pause = 0,
  IconType_Play = 1,
  IconType_NextFrame = 2,
  IconType_End = 3,
  IconType_Folder = 4,
  IconType_Up = 5,
  IconType_Count = 6,
};

#define MAX_UNDO_BUFFER (32)
typedef struct {
  VideoClips states[MAX_UNDO_BUFFER];
  int head, tail, pos;
} UndoBuffer;

void undobuffer_clear(UndoBuffer* buffer) {
  for (int i = 0; i < MAX_UNDO_BUFFER; i++) {
    videoclips_free(&buffer->states[i]);
  }
  *buffer = (UndoBuffer){0};
}
void undobuffer_push(UndoBuffer* buffer, const VideoClips* clips) {
  if (buffer->pos != buffer->tail) {
    buffer->tail = buffer->pos;
  }
  buffer->tail++;
  VideoClips* undostate = &buffer->states[buffer->tail % MAX_UNDO_BUFFER];
  if (undostate->clips) {
    free(undostate->clips);
  }
  buffer->pos = buffer->tail;
  *undostate = (VideoClips){.clips = (VideoClip*)malloc(clips->num * sizeof(VideoClip)), .num = clips->num};
  assert(undostate->clips);
  memcpy(undostate->clips, clips->clips, sizeof(VideoClip) * clips->num);
}
void undobuffer_undo(UndoBuffer* buffer, VideoClips* clips) {
  if (buffer->pos == buffer->head) {
    return;
  }
  if (buffer->pos != buffer->tail && buffer->pos % MAX_UNDO_BUFFER == buffer->tail % MAX_UNDO_BUFFER) {
    return;
  }
  buffer->pos--;
  VideoClips* undostate = &buffer->states[buffer->pos % MAX_UNDO_BUFFER];
  assert(undostate->clips);
  if (clips->cap < undostate->num) {
    clips->clips = realloc(clips->clips, sizeof(VideoClip) * undostate->num);
    assert(clips->clips);
    clips->cap = undostate->num;
  }
  clips->num = undostate->num;
  memcpy(clips->clips, undostate->clips, sizeof(VideoClip) * undostate->num);
}
void undobuffer_redo(UndoBuffer* buffer, VideoClips* clips) {
  if (buffer->pos == buffer->tail) {
    return;
  }
  buffer->pos++;
  VideoClips* undostate = &buffer->states[buffer->pos % MAX_UNDO_BUFFER];
  assert(undostate->clips);
  if (clips->cap < undostate->num) {
    clips->clips = realloc(clips->clips, sizeof(VideoClip) * undostate->num);
    assert(clips->clips);
    clips->cap = undostate->num;
  }
  clips->num = undostate->num;
  memcpy(clips->clips, undostate->clips, sizeof(VideoClip) * undostate->num);
}

typedef struct {
  bool is_dir;
  sg_image thumbnail;
  char filename[PATH_MAX];
  double video_total_secs;
} VideoSource;

typedef struct {
  VideoSource* sources;
  int num, cap;
  char filepath[PATH_MAX];
} VideoSources;

static const char* file_formats[] = {
    "webm", "mkv", "flv", "vob", "ogv", "ogg", "rrc", "gifv", "mng", "mov",  "avi", "qt",  "wmv",
    "yuv",  "rm",  "asf", "amv", "mp4", "m4p", "m4v", "mpg",  "mp2", "mpeg", "mpe", "mpv", "m4v",
    "svi",  "3gp", "3g2", "mxf", "roq", "nsv", "flv", "f4v",  "f4p", "f4a",  "f4b", "mod",
};

void videosources_opendir(VideoSources* s, const char* path) {
  // destroy any thumbnails
  for (int i = 0; i < s->num; i++) {
    if (s->sources[i].thumbnail.id) {
      sg_destroy_image(s->sources[i].thumbnail);
    }
  }
  s->num = 0;
  // open the directory and list the files
  DIR* dir = opendir(path);
  if (dir == NULL) {
    return;
  }
  struct dirent* ent;
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_type != DT_DIR && ent->d_type != DT_REG) {
      continue; // only look at files and directories
    }
    sg_image thumbnail = (sg_image){0};
    double vid_total_secs = 0.0;
    if (ent->d_type == DT_REG) {
      char* dot = strrchr(ent->d_name, '.');
      if (dot == NULL) {
        continue;
      }
      bool ok_format = false;
      for (int i = 0; i < _countof(file_formats); i++) {
        if (_strcmpi(dot + 1, file_formats[i]) == 0) {
          ok_format = true;
          break;
        }
      }
      if (!ok_format) {
        continue;
      }
      char fullpath[PATH_MAX];
      snprintf(fullpath, PATH_MAX, "%s/%s", path, ent->d_name);
      VideoOpenRes res = video_open(fullpath);
      if (res.err) {
        DebugLog("failed to open %s: %s", fullpath, res.err);
        continue;
      }
      thumbnail = video_make_thumbnail(res.vid, 0.0, 100, 100);
      vid_total_secs = video_total_secs(res.vid);
      video_close(res.vid);
    } else {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }
    }
    if (s->num + 1 >= s->cap) {
      s->cap = s->cap ? s->cap * 2 : 32;
      void* newbuf = realloc(s->sources, sizeof(VideoSource) * s->cap);
      assert(newbuf);
      s->sources = (VideoSource*)newbuf;
    }
    VideoSource* source = &s->sources[s->num++];
    *source =
        (VideoSource){.is_dir = ent->d_type == DT_DIR, .thumbnail = thumbnail, .video_total_secs = vid_total_secs};
    snprintf(source->filename, PATH_MAX, "%s", ent->d_name);
  }
  snprintf(s->filepath, PATH_MAX, "%s", path);
  closedir(dir);
}

typedef struct MovieMaker {
  FONScontext* font_ctx;
  int font_sans, font_mono, font_mono_bold;
  UI* ui;
  bool paused;

  UndoBuffer undo;

  VideoClips clips;
  double trackpos, tracklen;
  bool didseektrack;

  int selclipidx;
  float selclipdragstart;
  double selclipdragstartoffset;
  bool selclipdragstarted;

  float trackoffset, trackzoom, trackzoomspeed, trackdragstart;
  double trackdragstartoffset;
  bool trackdragstarted;

  VideoSources sources;
  float sourcescroll;
  const VideoSource *dragvideo, *placevideo;
  Rect dragvideopos;

  sg_image icons;
  Rect iconrects[IconType_Count];
  Rect iconrectshflipped[IconType_Count];
} MovieMaker;
MovieMaker state;

extern unsigned char VeraMono_ttf[], Vera_ttf[], VeraMono_Bold_ttf[], icons_png[];
extern unsigned int Vera_ttf_len, VeraMono_ttf_len, VeraMono_Bold_ttf_len, icons_png_len;

/* round to next power of 2 (see bit-twiddling-hacks) */
static int round_pow2(float v) {
  uint32_t vi = ((uint32_t)v) - 1;
  for (uint32_t i = 0; i < 5; i++) {
    vi |= (vi >> (1 << i));
  }
  return (int)(vi + 1);
}

int fonsAddFontMem(FONScontext* stash, const char* name, unsigned char* data, int dataSize, int freeData);

static void app_init(void) {
  sg_setup(&(sg_desc){.context = sapp_sgcontext()});
  sgl_setup(&(sgl_desc_t){0});
  videopool_init();

  MovieMaker* m = &state;
  const int atlas_dim = round_pow2(512.0f * sapp_dpi_scale());
  m->font_ctx = sfons_create(atlas_dim, atlas_dim, FONS_ZERO_TOPLEFT);
  m->font_sans = fonsAddFontMem(m->font_ctx, "sans", Vera_ttf, Vera_ttf_len, false);
  m->font_mono = fonsAddFontMem(m->font_ctx, "mono", VeraMono_ttf, VeraMono_ttf_len, false);
  m->font_mono_bold = fonsAddFontMem(m->font_ctx, "monob", VeraMono_Bold_ttf, VeraMono_Bold_ttf_len, false);
  m->ui = ui_init(sapp_dpi_scale(), m->font_ctx, m->font_sans);

  {
    int w, h, chans = 4;
    stbi_uc* icons_png_data = stbi_load_from_memory(icons_png, icons_png_len, &w, &h, &chans, 4);
    m->icons = sg_make_image(&(sg_image_desc){.width = w,
                                              .height = h,
                                              .pixel_format = SG_PIXELFORMAT_RGBA8,
                                              .data.subimage[0][0] = {.ptr = icons_png_data, .size = w * h * 4}});
    stbi_image_free(icons_png_data);
    for (int i = 0; i < IconType_Count; i++) {
      Rect r = (Rect){(float)(i * h) / (float)w, 0.0f, (float)((i + 1) * h) / (float)w, 1.0f};
      m->iconrects[i] = r;
      m->iconrectshflipped[i] = (Rect){r.maxx, r.miny, r.minx, r.maxy};
    }
  }

  m->trackzoom = 800.0f / 32.0f;
  m->trackoffset = 16.0f * 0.5f;
  m->tracklen = 16.0f * 2.0f;
  m->selclipidx = -1;
  undobuffer_clear(&m->undo);

  char cwd[PATH_MAX];
  GetCurrentDirectoryA(PATH_MAX, cwd);
  videosources_opendir(&m->sources, cwd);
}

static void app_sliceclip(void) {
  MovieMaker* m = &state;
  if (m->selclipidx == -1) {
    return;
  }
  VideoClip* clip = &m->clips.clips[m->selclipidx];
  double pos_secs = m->trackpos;
  if (clip->pos > pos_secs || pos_secs > (clip->pos + clip->clipend - clip->clipstart)) {
    return;
  }
  sg_image thumbnail = video_make_thumbnail(clip->vid, (pos_secs - clip->pos) + clip->clipstart, 100, 100);
  videoclips_push(&m->clips, (VideoClip){.pos = pos_secs,
                                         .track = clip->track,
                                         .clipstart = (pos_secs - clip->pos) + clip->clipstart,
                                         .clipend = clip->clipend,
                                         .vid = clip->vid,
                                         .thumbnail = thumbnail});
  clip->clipend = pos_secs - clip->pos + clip->clipstart;
  undobuffer_push(&m->undo, &m->clips);
}

static void app_deleteclip(void) {
  MovieMaker* m = &state;
  if (m->selclipidx == -1) {
    return;
  }
  m->clips.clips[m->selclipidx] = m->clips.clips[m->clips.num - 1];
  m->clips.num--;
  m->selclipidx = -1;
  undobuffer_push(&m->undo, &m->clips);
}

static void app_event(const sapp_event* ev) {
  MovieMaker* m = &state;
  ui_handle_event(m->ui, ev);
  switch (ev->type) {
  case SAPP_EVENTTYPE_KEY_DOWN:
    switch (ev->key_code) {
    case SAPP_KEYCODE_X:
      app_sliceclip();
      break;
    case SAPP_KEYCODE_DELETE:
      app_deleteclip();
      break;
    case SAPP_KEYCODE_Z:
      if (ev->modifiers & SAPP_MODIFIER_CTRL) {
        if (ev->modifiers & SAPP_MODIFIER_SHIFT) {
          undobuffer_redo(&m->undo, &m->clips);
        } else {
          undobuffer_undo(&m->undo, &m->clips);
        }
      }
      break;
    case SAPP_KEYCODE_Y:
      if (ev->modifiers & SAPP_MODIFIER_CTRL) {
        undobuffer_redo(&m->undo, &m->clips);
      }
      break;
    case SAPP_KEYCODE_O:
      if (ev->modifiers & SAPP_MODIFIER_CTRL) {
        undobuffer_clear(&m->undo);
        videoclips_free(&m->clips);
        m->trackpos = 0.0;
        m->trackzoom = 800.0f / 32.0f;
        m->trackoffset = 16.0f * 0.5f;
        videoclips_load("project.json", &m->clips);
      }
      break;
    case SAPP_KEYCODE_S:
      if (ev->modifiers & SAPP_MODIFIER_CTRL) {
        videoclips_save("project.json", &m->clips);
      }
    }
    break;
  }
}

Color bg_col = {24, 24, 24, 255};
Color videobg_col = {34, 34, 34, 255};
Color panel_col = {48, 48, 48, 255};
Color button_col = {84, 84, 84, 255};
BoxStyle button = {.bg_color = {84, 84, 84, 255}, .border_radius = 1.0f};
BoxStyle button_highlight = {.bg_color = {101, 101, 101, 255}, .border_radius = 1.0f};
BoxStyle button_down = {.bg_color = {60, 60, 60, 255}, .border_radius = 1.0f};
Color blue_col = {71, 114, 179, 255};
Color lightblue_col = {77, 100, 144, 255};

Color track_bg = {29, 29, 29, 255};
BoxStyle track_style = {.bg_color = {51, 77, 128, 255}, .border_radius = 1.0f};
BoxStyle track_style_shadow = {.bg_color = {0, 0, 0, 255}, .border_radius = 1.0f, .blur_amount = 0.1f};
BoxStyle track_style_sel = {.bg_color = {77, 100, 144, 255}, .border_radius = 1.0f};
Color trackmarker_col = {66, 109, 174, 255};

static void app_trackspanel(MovieMaker* m, Rect trackspanel) {

  ui_draw_box(m->ui, trackspanel, &(BoxStyle){.bg_color = track_bg});
  Rect timebar = rect_cut_top(&trackspanel, 24.0f);

  // figure out the total movie length
  m->tracklen = 0.0;
  for (int i = 0; i < m->clips.num; i++) {
    VideoClip* clip = &m->clips.clips[i];
    double clipend = clip->pos + (clip->clipend - clip->clipstart);
    if (clipend > m->tracklen) {
      m->tracklen = clipend;
    }
  }
  if (ui_get_event(m->ui, timebar) & UIEvent_MouseDown) {
    m->selclipidx = -1;
    m->trackpos = ui_clampd((ui_mouse(m->ui, timebar).x / m->trackzoom) - m->trackoffset, 0.0, m->tracklen);
    m->didseektrack = true;
    m->paused = true;
  }

  // trackzoom is second width in pixels
  // trackoffset is in seconds
  int unitidx = 0;
  static float units[] = {1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f, 300.0f};

  // draw the units and the tick bars
  bool wantframes = false;
  float unitwidth = (float)m->trackzoom;
  if (unitwidth > 1300.0f) {
    unitwidth /= 30.0f;
    wantframes = true;
  } else if (unitwidth > 300.0f) {
    unitwidth /= 2.0f;
  } else {
    while (unitwidth < 60.0f && unitidx < 7) {
      unitwidth = (float)m->trackzoom * units[unitidx++];
    }
  }
  float startx = (float)fmod(m->trackoffset * m->trackzoom, unitwidth) - unitwidth;
  float width = rect_width(trackspanel);
  for (float x = startx; x < width; x += unitwidth) {
    float x1 = x + unitwidth;
    double t = (x / m->trackzoom) - m->trackoffset;
    char buf[256];
    if (wantframes) {
      snprintf(buf, 256, "%d", (int)round(t * 30.0f));
    } else {
      if (fabs(t) < 0.4) {
        t = 0.0;
      }
      snprintf(buf, 256, "%g", round(t * 2.0f) * 0.5f);
    }
    ui_draw_text(m->ui, rect_translate((Rect){x, timebar.miny, x1, timebar.maxy}, -5.0f, 5.0f), buf, NULL,
                 &(DrawTextOptions){.font_size = 14.0f});
    ui_draw_box(m->ui, (Rect){x, trackspanel.miny + 1.0f, x1 - 1.0f, trackspanel.maxy - 1.0f},
                &(BoxStyle){.bg_color = panel_col});
  }

  // highlight the track area in use
  ui_draw_box(m->ui,
              (Rect){(float)(m->trackoffset * m->trackzoom), trackspanel.miny,
                     (float)((m->tracklen + m->trackoffset) * m->trackzoom), trackspanel.maxy},
              &(BoxStyle){255, 255, 255, 20});

  // grab track event for underneath the tracks
  UIEvent trackevt = ui_get_event(m->ui, trackspanel);
  if (trackevt & UIEvent_MouseDown) { // clear selection and time drag
    m->selclipidx = -1;
  }

  // draw each track
  for (int i = 0; i < m->clips.num; i++) {
    VideoClip* clip = &m->clips.clips[i];
    float x0 = (float)((clip->pos + m->trackoffset) * m->trackzoom);
    float x1 = (float)((clip->pos + (clip->clipend - clip->clipstart) + m->trackoffset) * m->trackzoom);
    Rect track = (Rect){x0, trackspanel.miny + 5.0f + (100.0f * clip->track), x1,
                        trackspanel.miny + 90.0f + (100.0f * clip->track)};
    UIEvent clipevt = ui_get_event(m->ui, track);
    trackevt |= clipevt & (UIEvent_MouseDrag | UIEvent_MouseMidDrag | UIEvent_MouseMidDown | UIEvent_MouseHover);
    if (clipevt & UIEvent_MouseDown) {
      if (clipevt & UIEvent_MouseDrag && m->selclipidx == i) {
        Mouse mouse = ui_mouse(m->ui, timebar);
        if (!m->selclipdragstarted) {
          m->selclipdragstarted = true;
          m->selclipdragstart = mouse.x;
          m->selclipdragstartoffset = clip->pos;
        }
        double prevpos = clip->pos;
        int prevtrack = clip->track;
        clip->pos = ((mouse.x - m->selclipdragstart) / m->trackzoom) + m->selclipdragstartoffset;
        clip->track = mouse.y > 95.0f + rect_height(timebar);
        double clipposend = clip->pos + (clip->clipend - clip->clipstart);
        // prevent overlaps with a single clip
        for (int j = 0; j < m->clips.num; j++) {
          if (i == j) {
            continue;
          }
          VideoClip* o = &m->clips.clips[j];
          if (o->track != clip->track) {
            continue;
          }
          double oposend = o->pos + (o->clipend - o->clipstart);
          if (o->pos <= clip->pos && clip->pos <= oposend) {
            clip->pos = oposend;
            break;
          }
          if (o->pos <= clipposend && clipposend <= oposend) {
            clip->pos = o->pos - (clip->clipend - clip->clipstart);
            break;
          }
        }
        if (clip->pos < 0.0) {
          clip->pos = 0.0;
        }
        // if we're still overlapping return to the original position
        for (int j = 0; j < m->clips.num; j++) {
          if (i == j) {
            continue;
          }
          VideoClip* o = &m->clips.clips[j];
          if (o->track != clip->track) {
            continue;
          }
          double oposend = o->pos + (o->clipend - o->clipstart);
          if ((o->pos < clip->pos && clip->pos < oposend) || (o->pos < clipposend && clipposend < oposend) ||
              (clip->pos < o->pos && clipposend > o->pos)) {
            clip->pos = prevpos;
            clip->track = prevtrack;
            break;
          }
        }
      }
      m->selclipidx = i;
    } else if (m->selclipidx == i && m->selclipdragstarted) {
      m->selclipdragstarted = false;
      undobuffer_push(&m->undo, &m->clips);
    }

    ui_draw_box(m->ui, rect_translate(rect_expand(track, 3.0f), 1.0f, 1.0f), &track_style_shadow);
    ui_draw_box(m->ui, track, i == m->selclipidx ? &track_style_sel : &track_style);
    track = rect_contract(track, 5.0f);
    Rect clipname = rect_cut_top(&track, 15.0f);
    if (rect_width(clipname) > 2.0f) {
      ui_scissor(m->ui, &clipname);
      ui_draw_text(m->ui, clipname, video_filename(clip->vid), NULL, &(DrawTextOptions){.font_size = 14.0f});
      ui_scissor(m->ui, NULL);
      ui_draw_image(m->ui, rect_inset_left(track, 80.0f), clip->thumbnail, (Rect){0.0f, 0.0f, 1.0f, 1.0f});
    }
  }

  // apply track zoom, track middle mouse drag, selection clearing, time drag
  if (trackevt & UIEvent_MouseHover) { // track zoom
    float midx = trackspanel.minx + rect_width(trackspanel) * 0.5f;
    float midt = (midx / m->trackzoom);
    m->trackzoom = ui_clamp(m->trackzoom + ui_mousescroll(m->ui).dy * 0.002f * m->trackzoom, 1.0f, 2000.0f);
    float newmidt = (midx / m->trackzoom);
    m->trackoffset += (newmidt - midt);
  }
  if (trackevt & UIEvent_MouseMidDown) { // track movement
    Mouse mouse = ui_mouse(m->ui, timebar);
    if (trackevt & UIEvent_MouseMidDrag) {
      if (!m->trackdragstarted) {
        m->trackdragstarted = true;
        m->trackdragstart = mouse.x;
        m->trackdragstartoffset = m->trackoffset;
      }
      m->trackoffset = (float)(((mouse.x - m->trackdragstart) / m->trackzoom) + m->trackdragstartoffset);
    }
  } else {
    m->trackdragstarted = false;
  }

  // draw clip being placed
  if (m->dragvideo || m->placevideo) {
    const VideoSource* source = m->dragvideo ? m->dragvideo : m->placevideo;
    double pos = ((m->dragvideopos.minx - trackspanel.minx) / m->trackzoom) - m->trackoffset;
    if (pos < 0.0) {
      pos = 0.0;
    }
    double posend = pos + source->video_total_secs;
    float posy = m->dragvideopos.miny - trackspanel.miny;
    if (posy > 0.0f) {
      int trackidx = posy > 95.0f + rect_height(timebar);
      // prevent overlaps with a single clip
      for (int j = 0; j < m->clips.num; j++) {
        VideoClip* o = &m->clips.clips[j];
        if (o->track != trackidx) {
          continue;
        }
        double oposend = o->pos + (o->clipend - o->clipstart);
        if (o->pos < pos && pos < oposend) {
          pos = oposend;
          break;
        }
        if (o->pos < posend && posend < oposend) {
          pos = o->pos - source->video_total_secs;
          break;
        }
      }
      // make sure clip doesn't collide with an existing clip
      bool clip_overlaps = false;
      for (int i = 0; i < m->clips.num; i++) {
        VideoClip* o = &m->clips.clips[i];
        if (o->track != trackidx) {
          continue;
        }
        double oposend = o->pos + (o->clipend - o->clipstart);
        if ((o->pos <= pos && pos <= oposend) || (o->pos <= posend && posend <= oposend) ||
            (pos <= o->pos && posend >= o->pos)) {
          clip_overlaps = true;
          break;
        }
      }
      // draw the clip if it doesn't overlap
      if (!clip_overlaps) {
        float x0 = (float)((pos + m->trackoffset) * m->trackzoom);
        float x1 = (float)((pos + source->video_total_secs + m->trackoffset) * m->trackzoom);
        Rect track = (Rect){x0, trackspanel.miny + 5.0f + (100.0f * trackidx), x1,
                            trackspanel.miny + 90.0f + (100.0f * trackidx)};
        ui_draw_box(m->ui, rect_translate(rect_expand(track, 3.0f), 1.0f, 1.0f), &track_style_shadow);
        ui_draw_box(m->ui, track, &track_style_sel);
        track = rect_contract(track, 5.0f);
        Rect clipname = rect_cut_top(&track, 15.0f);
        if (rect_width(clipname) > 2.0f) {
          ui_scissor(m->ui, &clipname);
          ui_draw_text(m->ui, clipname, source->filename, NULL, &(DrawTextOptions){.font_size = 14.0f});
          ui_scissor(m->ui, NULL);
          ui_draw_image(m->ui, rect_inset_left(track, 80.0f), source->thumbnail, (Rect){0.0f, 0.0f, 1.0f, 1.0f});
        }
        // dragging has stopped and we're in a valid position: actually place the video!
        if (m->placevideo) {
          char fullpath[PATH_MAX];
          snprintf(fullpath, PATH_MAX, "%s/%s", m->sources.filepath, m->placevideo->filename);
          VideoOpenRes res = video_open(fullpath);
          if (res.err) {
            DebugLog("failed to open video %s: %s\n", fullpath, res.err);
          } else {
            sg_image thumbnail = video_make_thumbnail(res.vid, 0.0, 100, 100);
            videoclips_push(&m->clips, (VideoClip){.vid = res.vid,
                                                   .pos = pos,
                                                   .clipend = source->video_total_secs,
                                                   .thumbnail = thumbnail,
                                                   .track = trackidx});
            undobuffer_push(&m->undo, &m->clips);
          }
        }
      }
    }
  }

  // draw the position marker
  {
    double t = m->trackpos;
    float pos = (float)((t + m->trackoffset) * m->trackzoom);

    Rect marker = (Rect){pos - 1.0f, trackspanel.miny, pos + 1.0f, trackspanel.maxy};
    ui_draw_box(m->ui, rect_expand(marker, 1.0f), &(BoxStyle){.bg_color = {0, 0, 0, 255}});
    ui_draw_box(m->ui, marker, &(BoxStyle){.bg_color = trackmarker_col});

    Rect posmarker = (Rect){timebar.minx - 25.0f + pos, timebar.miny, timebar.minx + 25.0f + pos, timebar.maxy};
    ui_draw_box(m->ui, posmarker, &(BoxStyle){.bg_color = trackmarker_col, .border_radius = 1.0f});
    char buf[256];
    snprintf(buf, 256, "%.02f", t);
    ui_draw_text(m->ui, rect_translate(posmarker, 8.0f, 5.0f), buf, NULL, &(DrawTextOptions){.font_size = 14.0f});
  }
}

static void app_drawvideo(MovieMaker* m, VideoId vid, Rect videopanel) {
  float vw = (float)video_width(vid), vh = (float)video_height(vid);
  float rw = rect_width(videopanel), rh = rect_height(videopanel);
  float w, h;
  if (vh > vw) {
    float aspect = vw / vh;
    w = aspect * rh;
    h = rh;
  } else {
    float aspect = vh / vw;
    w = rw;
    h = aspect * rw;
  }
  if (h > rh) {
    float scale = rh / h;
    w *= scale;
    h *= scale;
  }
  if (w > rw) {
    float scale = rw / w;
    w *= scale;
    h *= scale;
  }
  Rect video = rect_centre(videopanel, w, h);
  ui_draw_box(m->ui, video, &button);
  ui_draw_image(m->ui, video, video_image(vid), (Rect){.maxx = 1.0f, .maxy = 1.0f});
}

static void app_videopanel(MovieMaker* m, Rect videopanel) {
  ui_draw_box(m->ui, videopanel, &(BoxStyle){.bg_color = videobg_col});
  videopanel = rect_contract(videopanel, 10.0f);
  Rect buttons = rect_cut_bottom(&videopanel, 30.0f);
  // draw play pause etc. buttons
  for (int i = 0; i < 3; i++) {
    Rect buttonpos = rect_cut_left(&buttons, 30.0f);
    UIEvent evt = ui_get_event(m->ui, buttonpos);
    ui_draw_box(m->ui, buttonpos,
                evt & UIEvent_MouseDown    ? &button_down
                : evt & UIEvent_MouseHover ? &button_highlight
                                           : &button);
    Rect r;
    switch (i) {
    default:
    case 0:
      r = m->iconrectshflipped[IconType_End];
      if (evt & UIEvent_MouseClick) {
        m->trackpos = 0.0;
        m->didseektrack = true;
      }
      break;
    case 1:
      r = m->iconrects[m->paused ? IconType_Play : IconType_Pause];
      if (evt & UIEvent_MouseClick) {
        m->paused = !m->paused;
      }
      break;
    case 2:
      r = m->iconrects[IconType_End];
      if (evt & UIEvent_MouseClick) {
        m->trackpos = m->tracklen;
        m->didseektrack = true;
      }
      break;
    }
    ui_draw_image(m->ui, buttonpos, m->icons, r);
    rect_cut_left(&buttons, 5.0f);
  }
  rect_cut_left(&buttons, 5.0f);
  // draw time label
  {
    Rect timedisplay = buttons;
    char buf[256];
    snprintf(buf, 256, "%00.3f/%00.3f", m->trackpos, m->tracklen);
    timedisplay.miny += 8.0f;
    ui_draw_text(m->ui, timedisplay, buf, NULL,
                 &(DrawTextOptions){.align = TextAlign_Middle | TextAlign_Left, .font_size = 14.0f});
  }
  rect_cut_bottom(&videopanel, 10.0f);
  // draw playbar
  {
    Rect playbar = rect_cut_bottom(&videopanel, 10.0f);
    ui_draw_box(m->ui, playbar, &(BoxStyle){.bg_color = bg_col, .blur_amount = 0.03f});
    UIEvent evt = ui_get_event(m->ui, playbar);
    if (evt & UIEvent_MouseDown) {
      float dx = ui_clamp(ui_mouse(m->ui, playbar).x / rect_width(playbar), 0.0f, 1.0f);
      m->trackpos = dx * m->tracklen;
      m->didseektrack = true;
      m->paused = true;
    }
    playbar = rect_contract(playbar, 2.0f);
    ui_draw_box(m->ui, playbar, &(BoxStyle){.bg_color = button_col});
    if (video_total_secs) {
      Rect progress =
          rect_cut_left(&playbar, rect_width(playbar) * (float)(m->trackpos / (m->tracklen ? m->tracklen : 1.0)));
      ui_draw_box(m->ui, progress, &(BoxStyle){.bg_color = blue_col});
      Rect marker = rect_cut_right(&progress, 2.0f);
      marker.miny -= 4.0f;
      marker.maxy += 4.0f;
      marker.maxx += 2.0f;
      ui_draw_box(m->ui, rect_expand(marker, 2.0f), &(BoxStyle){.bg_color = bg_col, .blur_amount = 0.5f});
      ui_draw_box(m->ui, marker,
                  &(BoxStyle){.bg_color = (evt & (UIEvent_MouseDown | UIEvent_MouseHover)) ? lightblue_col : blue_col});
    }
  }
  rect_cut_bottom(&videopanel, 10.0f);
  // draw video
  {
    ui_draw_box(m->ui, videopanel, &(BoxStyle){.bg_color = {0, 0, 0, 255}});
    double dt = sapp_frame_duration();
    if (!m->paused) {
      m->trackpos = ui_clampd(m->trackpos + dt, 0.0, m->tracklen);
    }

    // find the current top and bottom clip
    const VideoClip* top = NULL;
    const VideoClip* bottom = NULL;
    for (int i = 0; i < m->clips.num; i++) {
      const VideoClip* clip = &m->clips.clips[i];
      if (m->trackpos >= clip->pos && m->trackpos <= clip->pos + (clip->clipend - clip->clipstart)) {
        if (clip->track == 0 && top == NULL) {
          top = clip;
        }
        if (clip->track == 1 && bottom == NULL) {
          bottom = clip;
        }
        if (top && bottom) {
          break;
        }
      }
    }
    if (top) {
      double clippos = ui_clampd(m->trackpos - top->pos + top->clipstart, 0.0, video_total_secs(top->vid));
      video_nextframe(top->vid, clippos);
      app_drawvideo(m, top->vid, videopanel);
    } else if (bottom) {
      double clippos = ui_clampd(m->trackpos - bottom->pos + bottom->clipstart, 0.0, video_total_secs(bottom->vid));
      video_nextframe(bottom->vid, clippos);
      app_drawvideo(m, bottom->vid, videopanel);
    }
  }
}

static void app_sourcepanel(MovieMaker* m, Rect sourcepanel) {
  ui_draw_box(m->ui, sourcepanel, &(BoxStyle){.bg_color = panel_col});
  sourcepanel = rect_contract(sourcepanel, 5.0f);
  Rect topbar = rect_cut_top(&sourcepanel, 36.0f);
  {
    // draw up button
    Rect upbutton = rect_contract(rect_cut_left(&topbar, 36.0f), 3.0f);
    UIEvent evt = ui_get_event(m->ui, upbutton);
    ui_draw_box(m->ui, upbutton,
                evt & UIEvent_MouseHover ? &(BoxStyle){.bg_color = {101, 101, 101, 255}, .border_radius = 0.2f}
                                         : &(BoxStyle){.bg_color = {84, 84, 84, 255}, .border_radius = 0.2f});
    ui_draw_image(m->ui, rect_centre(upbutton, 30.0f, 30.0f), m->icons, m->iconrects[IconType_Up]);
    if (evt & UIEvent_MouseClick) {
      char newfilepath[MAX_PATH];
      char* firstsep = strchr(m->sources.filepath, '/');
      if (firstsep == NULL) {
        firstsep = strchr(m->sources.filepath, '\\');
      }
      char* lastsep = strrchr(m->sources.filepath, '/');
      if (lastsep == NULL) {
        lastsep = strrchr(m->sources.filepath, '\\');
      }
      if (lastsep) {
        if (firstsep == lastsep) {
          lastsep += 1;
        }
        m->sourcescroll = 0.0f;
        snprintf(newfilepath, MAX_PATH, "%.*s", (int)(lastsep - m->sources.filepath), m->sources.filepath);
        videosources_opendir(&m->sources, newfilepath);
      }
    }

    // draw filepath
    Rect filepath = rect_contract(topbar, 5.0f);
    ui_draw_box(m->ui, filepath, &(BoxStyle){.bg_color = {61, 61, 61, 255}, .border_radius = 0.2f});
    ui_draw_box(m->ui, rect_contract(filepath, 1.0f),
                &(BoxStyle){.bg_color = {29, 29, 29, 255}, .border_radius = 0.2f});
    ui_scissor(m->ui, &filepath);
    ui_draw_text(m->ui, rect_translate(rect_contract(filepath, 2.0f), 0.0f, 5.0f), m->sources.filepath, NULL,
                 &(DrawTextOptions){.font_size = 14.0f});
    ui_scissor(m->ui, NULL);
  }
  // draw the video + folder grid
  rect_cut_top(&sourcepanel, 5.0f);
  ui_draw_box(m->ui, sourcepanel, &(BoxStyle){.bg_color = {40, 40, 40, 255}});
  Rect sourcepanelscissor = sourcepanel;
  ui_scissor(m->ui, &sourcepanelscissor);
  bool wantscroll = false;
  if (ui_get_event(m->ui, sourcepanel) & UIEvent_MouseHover) {
    wantscroll = true;
  }
  int gx = 0, gy = 0;
  float sourcepanelwidth = rect_width(sourcepanel);
  const float gridwidth = 100.0f;
  float maxscroll = gridwidth;
  m->placevideo = m->dragvideo;
  m->dragvideo = NULL;
  for (int i = 0; i < m->sources.num; i++) {
    if ((gx + 1) * gridwidth > sourcepanelwidth) {
      gy++;
      maxscroll += gridwidth;
      gx = 0;
    }
    const VideoSource* source = &m->sources.sources[i];
    float mx = sourcepanel.minx + (gx * gridwidth), my = sourcepanel.miny + (gy * gridwidth) - m->sourcescroll;
    Rect grid = rect_contract((Rect){mx, my, mx + gridwidth, my + gridwidth}, 5.0f);
    UIEvent evt = ui_get_event(m->ui, grid);
    ui_draw_box(m->ui, grid,
                evt & UIEvent_MouseHover ? &(BoxStyle){.bg_color = 84, 84, 84, 255, .border_radius = 0.5f}
                                         : &(BoxStyle){.bg_color = 64, 64, 64, 255, .border_radius = 0.5f});
    if (evt & UIEvent_MouseHover) {
      wantscroll = true;
    }
    if (evt & UIEvent_MouseClick && source->is_dir) {
      m->sourcescroll = 0.0f;
      char newfilepath[MAX_PATH];
#ifdef _WIN32
      snprintf(newfilepath, MAX_PATH, "%s\\%s", m->sources.filepath, source->filename);
#else
      snprintf(newfilepath, MAX_PATH, "%s/%s", m->sources.filepath, source->filename);
#endif
      videosources_opendir(&m->sources, newfilepath);
    }
    if (evt & UIEvent_MouseDrag) {
      m->dragvideo = source;
      m->placevideo = NULL;
      Mouse mouse = ui_mouse(m->ui, (Rect){0});
      m->dragvideopos = rect_contract((Rect){mouse.x, mouse.y, mouse.x + gridwidth, mouse.y + gridwidth}, 5.0f);
    }
    grid = rect_contract(grid, 2.0f);
    if (rect_contains(&sourcepanelscissor, grid.minx, grid.miny)) {
      ui_scissor(m->ui, &grid);
      ui_draw_text(m->ui, grid, source->filename, NULL, &(DrawTextOptions){.font_size = 14.0f});
      ui_scissor(m->ui, &sourcepanelscissor);
    }
    if (source->is_dir) {
      ui_draw_image(m->ui, rect_centre(grid, 30.0f, 30.0f), m->icons, m->iconrects[IconType_Folder]);
    } else if (source->thumbnail.id) {
      rect_cut_top(&grid, 15.0f);
      ui_draw_image(m->ui, grid, source->thumbnail, (Rect){0.0f, 0.0f, 1.0f, 1.0f});
    }
    gx++;
  }
  if (m->sources.num == 0) {
    ui_draw_text(m->ui, rect_centre(sourcepanelscissor, 80.0f, 30.0f), "No video files", NULL,
                 &(DrawTextOptions){.font_size = 14.0f});
  }
  // draw the scrollbar
  ui_scissor(m->ui, NULL);
  maxscroll -= rect_height(sourcepanelscissor);
  if (maxscroll <= 0.0f) {
    maxscroll = 0.0f;
  }
  if (wantscroll) {
    m->sourcescroll = ui_clamp(m->sourcescroll - ui_mousescroll(m->ui).dy * 0.5f, 0.0f, maxscroll);
  }
  Rect scrollbar = rect_contract(rect_inset_right(sourcepanelscissor, 15.0f), 5.0f);
  ui_draw_box(m->ui, scrollbar, &(BoxStyle){.bg_color = {54, 54, 54, 255}, .border_radius = 0.5f});
  float percent = m->sourcescroll / (maxscroll + rect_height(sourcepanelscissor));
  float percent2 = (m->sourcescroll + rect_height(sourcepanelscissor)) / (maxscroll + rect_height(sourcepanelscissor));
  float height = rect_height(scrollbar);
  scrollbar =
      (Rect){scrollbar.minx, scrollbar.miny + percent * height, scrollbar.maxx, scrollbar.miny + percent2 * height};
  ui_draw_box(m->ui, scrollbar, &(BoxStyle){.bg_color = {84, 84, 84, 255}, .border_radius = 0.5f});
}

static void app_frame(void) {
  MovieMaker* m = &state;

  ui_frame(m->ui);
  sgl_defaults();
  sgl_matrix_mode_projection();
  sgl_ortho(0.0f, sapp_widthf(), sapp_heightf(), 0.0f, -1.0f, +1.0f);

  Rect window = ui_windowrect(m->ui, sapp_widthf(), sapp_heightf());

  rect_cut_top(&window, 25.0f); // menu bar

  Rect trackspanel = rect_contract(rect_cut_bottom(&window, 256.0f), 1.0f);
  app_trackspanel(m, trackspanel);

  Rect sourcepanel = rect_contract(rect_cut_left(&window, 320.0f), 1.0f);
  app_sourcepanel(m, sourcepanel);

  Rect videopanel = rect_contract(window, 1.0f);
  app_videopanel(m, videopanel);

  if (m->dragvideo) {
    ui_draw_box(m->ui, m->dragvideopos, &(BoxStyle){.bg_color = 64, 64, 64, 255, .border_radius = 0.5f});
    Rect dragvideopos = rect_contract(m->dragvideopos, 2.0f);
    ui_scissor(m->ui, &dragvideopos);
    ui_draw_text(m->ui, dragvideopos, m->dragvideo->filename, NULL, &(DrawTextOptions){.font_size = 14.0f});
    ui_scissor(m->ui, NULL);
    if (m->dragvideo->thumbnail.id) {
      rect_cut_top(&dragvideopos, 15.0f);
      ui_draw_image(m->ui, dragvideopos, m->dragvideo->thumbnail, (Rect){0.0f, 0.0f, 1.0f, 1.0f});
    }
  }

  sfons_flush(m->font_ctx);
  sg_begin_default_pass(
      &(sg_pass_action){.colors[0] = {.action = SG_ACTION_CLEAR, .value = {0.113f, 0.113f, 0.113f, 1.0f}}},
      sapp_width(), sapp_height());
  sgl_draw();
  sg_end_pass();
  sg_commit();
}

static void app_cleanup(void) {
}

sapp_desc sokol_main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  return (sapp_desc){.init_cb = app_init,
                     .frame_cb = app_frame,
                     .event_cb = app_event,
                     .cleanup_cb = app_cleanup,
                     .width = 800,
                     .height = 600,
                     .sample_count = 4,
                     .gl_force_gles2 = true,
                     .window_title = "Filmsaw"};
}
