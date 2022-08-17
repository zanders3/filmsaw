#include "video.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <sokol/sokol_gfx.h>
#include <assert.h>

#pragma comment(lib, "src/3rdparty/ffmpeg/lib/avcodec.lib")
#pragma comment(lib, "src/3rdparty/ffmpeg/lib/avformat.lib")
#pragma comment(lib, "src/3rdparty/ffmpeg/lib/swscale.lib")
#pragma comment(lib, "src/3rdparty/ffmpeg/lib/swresample.lib")
#pragma comment(lib, "src/3rdparty/ffmpeg/lib/avutil.lib")

typedef struct {
  uint32_t id;

  AVFormatContext* fmt_ctx;
  AVCodecParameters* codec_params;
  AVCodecContext* codec_ctx;
  AVFrame *frame_raw, *frame_rgb;
  struct SwsContext* sws_ctx;
  sg_image img;
  uint8_t* imgbuf;
  int imgbuflen;
  int vidstreamidx;

  char filename[_MAX_PATH + 1];

  double pos_secs, next_swap_secs, total_secs;
  bool want_seek;
} Video;

typedef struct {
  int size;
  int queue_top;
  uint32_t* gen_ctrs;
  int* free_queue;
  Video* videos;
} VideoPool;
static VideoPool _videos;

void videopool_init() {
  VideoPool* pool = &_videos;
  pool->queue_top = 0;
  pool->size = VIDEO_POOL_SIZE + 1;
  size_t gen_ctrs_size = sizeof(uint32_t) * (size_t)pool->size;
  pool->gen_ctrs = (uint32_t*)malloc(gen_ctrs_size);
  assert(pool->gen_ctrs);
  memset(pool->gen_ctrs, 0, gen_ctrs_size);
  // it's not a bug to only reserve 'num' here
  pool->free_queue = (int*)malloc(sizeof(int) * (size_t)VIDEO_POOL_SIZE);
  assert(pool->free_queue);
  // never allocate the zero-th pool item since the invalid id is 0
  for (int i = pool->size - 1; i >= 1; i--) {
    pool->free_queue[pool->queue_top++] = i;
  }
  size_t pool_byte_size = sizeof(Video) * (size_t)pool->size;
  pool->videos = (Video*)malloc(pool_byte_size);
  assert(pool->videos);
  memset(pool->videos, 0, pool_byte_size);
}

#define _VIDEO_INVALIDID (0)
#define _VIDEO_INVALID_SLOT_INDEX (0)
#define _VIDEO_SLOT_SHIFT (16)
#define _VIDEO_MAX_POOL_SIZE (1 << _VIDEO_SLOT_SHIFT)
#define _VIDEO_SLOT_MASK (_VIDEO_MAX_POOL_SIZE - 1)

static VideoId _video_alloc() {
  VideoPool* pool = &_videos;
  if (pool->queue_top <= 0) {
    return (VideoId){0};
  }
  int slot_index = pool->free_queue[--pool->queue_top];
  assert((slot_index > 0) && (slot_index < pool->size));
  uint32_t ctr = ++pool->gen_ctrs[slot_index];
  Video* v = &pool->videos[slot_index];
  *v = (Video){.id = (ctr << _VIDEO_SLOT_SHIFT) | (slot_index & _VIDEO_SLOT_MASK)};
  return (VideoId){.id = v->id};
}
static Video* _video_at(VideoId id) {
  VideoPool* pool = &_videos;
  assert(_VIDEO_INVALIDID != id.id);
  int slot_index = (int)(id.id & _VIDEO_SLOT_MASK);
  assert(_VIDEO_INVALID_SLOT_INDEX != slot_index);
  assert((slot_index > _VIDEO_INVALID_SLOT_INDEX) && (slot_index < VIDEO_POOL_SIZE));
  Video* v = &pool->videos[slot_index];
  assert(v->id == id.id);
  return v;
}
static void _video_free(VideoId id) {
  VideoPool* pool = &_videos;
  Video* v = _video_at(id);
  int slot_index = (int)(id.id & _VIDEO_SLOT_MASK);
  assert(_VIDEO_INVALID_SLOT_INDEX != slot_index);
#ifdef _DEBUG
  /* debug check against double-free */
  for (int i = 0; i < pool->queue_top; i++) {
    assert(pool->free_queue[i] != slot_index);
  }
#endif
  pool->free_queue[pool->queue_top++] = slot_index;
  assert(pool->queue_top <= (pool->size - 1));
  *v = (Video){0};
}

VideoOpenRes video_open(const char* path) {
  VideoId vid = _video_alloc();
  Video* v = _video_at(vid);
  snprintf(v->filename, sizeof(v->filename), "%.*s", (int)strlen(path), path);

  const char* err = NULL;
  int res = avformat_open_input(&v->fmt_ctx, path, NULL, NULL);
  if (res != 0) {
    err = "Failed to open video";
    goto cleanup;
  }
  if (avformat_find_stream_info(v->fmt_ctx, NULL) < 0) {
    err = "Failed to find video stream info";
    goto cleanup;
  }
  for (uint32_t i = 0; i < v->fmt_ctx->nb_streams; i++) {
    if (v->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      v->codec_params = v->fmt_ctx->streams[i]->codecpar;
      v->total_secs = (double)v->fmt_ctx->duration * av_q2d(AV_TIME_BASE_Q);
      v->vidstreamidx = i;
      break;
    }
  }
  if (v->codec_params == NULL) {
    err = "Failed to find video stream";
    goto cleanup;
  }
  const AVCodec* codec = avcodec_find_decoder(v->codec_params->codec_id);
  if (codec == NULL) {
    err = "Unsupported video codec";
    goto cleanup;
  }
  v->codec_ctx = avcodec_alloc_context3(codec);
  if (v->codec_ctx == NULL) {
    err = "Unsupported video codec context";
    goto cleanup;
  }
  if (avcodec_parameters_to_context(v->codec_ctx, v->codec_params) != 0) {
    err = "Failed to setup codec";
    goto cleanup;
  }
  // Open codec
  if (avcodec_open2(v->codec_ctx, codec, NULL) < 0) {
    err = "Failed to open codec";
    goto cleanup;
  }
  v->frame_raw = av_frame_alloc();
  v->sws_ctx =
      sws_getContext(v->codec_params->width, v->codec_params->height, v->codec_params->format, v->codec_params->width,
                     v->codec_params->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
  v->img = sg_make_image(&(sg_image_desc){
      .width = v->codec_params->width,
      .height = v->codec_params->height,
      .pixel_format = SG_PIXELFORMAT_RGBA8,
      .usage = SG_USAGE_STREAM,
      .min_filter = SG_FILTER_LINEAR,
      .mag_filter = SG_FILTER_LINEAR,
      .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
      .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
  });
  v->imgbuflen = av_image_get_buffer_size(AV_PIX_FMT_RGBA, v->codec_params->width, v->codec_params->height, 1);
  v->imgbuf = av_malloc(v->imgbuflen);
  v->frame_rgb = av_frame_alloc();
  av_image_fill_arrays(v->frame_rgb->data, v->frame_rgb->linesize, v->imgbuf, AV_PIX_FMT_RGBA, v->codec_params->width,
                       v->codec_params->height, 1);
cleanup:
  if (err != NULL) {
    _video_free(vid);
    return (VideoOpenRes){.err = err};
  }
  return (VideoOpenRes){.vid = vid};
}

void video_seek(VideoId vid, double pos_secs) {
  Video* v = _video_at(vid);
  v->want_seek = true;
  if (pos_secs > v->total_secs) {
    pos_secs = v->total_secs;
  }
  if (pos_secs < 0.0) {
    pos_secs = 0.0;
  }
  v->pos_secs = pos_secs;
}

void video_nextframe(VideoId vid, double dt) {
  Video* v = _video_at(vid);
  if (v->want_seek) {
    int64_t timestamp =
        (int64_t)((double)v->pos_secs * av_q2d(av_inv_q(v->fmt_ctx->streams[v->vidstreamidx]->time_base)));
    av_seek_frame(v->fmt_ctx, v->vidstreamidx, timestamp, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(v->codec_ctx);
    v->next_swap_secs = 0.0f;
    AVPacket packet;
    double time_base = av_q2d(v->fmt_ctx->streams[v->vidstreamidx]->time_base);
    while (av_read_frame(v->fmt_ctx, &packet) >= 0) {
      if (avcodec_send_packet(v->codec_ctx, &packet) < 0) {
        av_packet_unref(&packet);
        continue;
      }
      if (avcodec_receive_frame(v->codec_ctx, v->frame_raw) >= 0) {
        v->next_swap_secs = (double)v->frame_raw->pts * time_base;
        if (v->next_swap_secs >= v->pos_secs) {
          sws_scale(v->sws_ctx, v->frame_raw->data, v->frame_raw->linesize, 0, v->codec_params->height,
                    v->frame_rgb->data, v->frame_rgb->linesize);
          sg_update_image(v->img, &(sg_image_data){.subimage[0][0] = {.ptr = v->imgbuf, .size = v->imgbuflen}});
          av_frame_unref(v->frame_raw);
          break;
        }
        av_frame_unref(v->frame_raw);
      }
      av_packet_unref(&packet);
    }
    av_packet_unref(&packet);
    v->want_seek = false;
    return;
  }
  v->pos_secs += dt;
  if (v->pos_secs > v->total_secs) {
    v->pos_secs = v->total_secs;
  }
  if (v->next_swap_secs > v->pos_secs) {
    return;
  }
  AVPacket packet;
  while (av_read_frame(v->fmt_ctx, &packet) >= 0) {
    if (avcodec_send_packet(v->codec_ctx, &packet) < 0) {
      av_packet_unref(&packet);
      continue;
    }
    if (avcodec_receive_frame(v->codec_ctx, v->frame_raw) >= 0) {
      sws_scale(v->sws_ctx, v->frame_raw->data, v->frame_raw->linesize, 0, v->codec_params->height, v->frame_rgb->data,
                v->frame_rgb->linesize);
      sg_update_image(v->img, &(sg_image_data){.subimage[0][0] = {.ptr = v->imgbuf, .size = v->imgbuflen}});
      v->next_swap_secs = (double)v->frame_raw->pts * av_q2d(v->fmt_ctx->streams[v->vidstreamidx]->time_base);
      av_frame_unref(v->frame_raw);
    }
    av_packet_unref(&packet);
    break;
  }
  av_packet_unref(&packet);
}

void video_close(VideoId vid) {
  Video* v = _video_at(vid);
  if (v->frame_raw) {
    av_frame_free(&v->frame_raw);
  }
  if (v->frame_rgb) {
    av_frame_free(&v->frame_rgb);
  }
  if (v->sws_ctx) {
    sws_freeContext(v->sws_ctx);
  }
  if (v->fmt_ctx) {
    avformat_close_input(&v->fmt_ctx);
  }
  sg_destroy_image(v->img);
  if (v->imgbuf) {
    av_free(v->imgbuf);
  }
  _video_free(vid);
}

double video_total_secs(VideoId vid) {
  return _video_at(vid)->total_secs;
}
double video_pos_secs(VideoId vid) {
  return _video_at(vid)->pos_secs;
}
int video_width(VideoId vid) {
  return _video_at(vid)->codec_params->width;
}
int video_height(VideoId vid) {
  return _video_at(vid)->codec_params->height;
}
sg_image video_image(VideoId vid) {
  return _video_at(vid)->img;
}
const char* video_filename(VideoId vid) {
  return _video_at(vid)->filename;
}

struct sg_image video_make_thumbnail(VideoId vid, double pos_secs, int width, int height) {
  Video* v = _video_at(vid);
  int64_t timestamp = (int64_t)((double)pos_secs * av_q2d(av_inv_q(v->fmt_ctx->streams[v->vidstreamidx]->time_base)));
  av_seek_frame(v->fmt_ctx, v->vidstreamidx, timestamp, AVSEEK_FLAG_BACKWARD);
  v->pos_secs = pos_secs;
  v->next_swap_secs = 0.0f;
  AVPacket packet;
  while (av_read_frame(v->fmt_ctx, &packet) >= 0) {
    if (avcodec_send_packet(v->codec_ctx, &packet) < 0) {
      continue;
    }
    if (avcodec_receive_frame(v->codec_ctx, v->frame_raw) >= 0) {
      struct SwsContext* sws_ctx =
          sws_getContext(v->codec_params->width, v->codec_params->height, v->codec_params->format, width, height,
                         AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
      int imgbuflen = av_image_get_buffer_size(AV_PIX_FMT_RGBA, width, height, 1);
      void* imgbuf = av_malloc(imgbuflen);
      AVFrame* frame_rgb = av_frame_alloc();
      av_image_fill_arrays(frame_rgb->data, frame_rgb->linesize, imgbuf, AV_PIX_FMT_RGBA, width, height, 1);
      sws_scale(sws_ctx, v->frame_raw->data, v->frame_raw->linesize, 0, v->codec_params->height, frame_rgb->data,
                frame_rgb->linesize);
      sg_image img = sg_make_image(&(sg_image_desc){
          .width = width,
          .height = height,
          .pixel_format = SG_PIXELFORMAT_RGBA8,
          .min_filter = SG_FILTER_LINEAR,
          .mag_filter = SG_FILTER_LINEAR,
          .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
          .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
          .data.subimage[0][0] = {.ptr = imgbuf, .size = imgbuflen},
      });
      av_free(imgbuf);
      sws_freeContext(sws_ctx);
      av_frame_free(&frame_rgb);
      return img;
    }
  }
  return (sg_image){0};
}
