#include "video.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <sokol/sokol_gfx.h>
#include <assert.h>
#include <thread/thread.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <sokol/sokol_audio.h>

#define MAX_QUEUE_LEN (256)

typedef struct {
  AVPacket* queue[MAX_QUEUE_LEN];
  int head, tail, num_packets;
} PacketQueue;

static bool packet_queue_full(PacketQueue* q) {
  return q->num_packets >= MAX_QUEUE_LEN;
}
static void packet_queue_put(PacketQueue* q, AVPacket* p) {
  assert(!packet_queue_full(q));
  int tailidx = q->tail % MAX_QUEUE_LEN;
  assert(tailidx < MAX_QUEUE_LEN && q->queue[tailidx] == NULL);
  q->queue[tailidx] = av_packet_alloc();
  av_packet_ref(q->queue[tailidx], p);
  q->tail++;
  q->num_packets++;
}
static AVPacket* packet_queue_pop(PacketQueue* q) {
  if (q->num_packets <= 0) {
    return NULL;
  }
  int headidx = q->head % MAX_QUEUE_LEN;
  assert(headidx < MAX_QUEUE_LEN && q->queue[headidx]);
  AVPacket* p = q->queue[headidx];
  q->queue[headidx] = NULL;
  q->head++;
  q->num_packets--;
  return p;
}
static void packet_queue_clear(PacketQueue* q) {
  for (int i = 0; i < MAX_QUEUE_LEN; i++) {
    if (q->queue[i]) {
      av_packet_unref(q->queue[i]);
    }
    q->queue[i] = NULL;
  }
  *q = (PacketQueue){0};
}

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

  AVFormatContext* aud_fmt_ctx;
  AVCodecParameters* aud_codec_params;
  AVCodecContext* aud_codec_ctx;
  int audiostreamidx;

  PacketQueue vid_queue;

  // protected by aud_thread_mtx
  PacketQueue aud_queue;
  AVFrame* aud_frame_raw;
  int aud_frame_pos;
  bool aud_got_frame, aud_playing;

  char filepath[_MAX_PATH + 1];

  double pos_secs, next_swap_secs, total_secs;
  bool gc_marked;
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
static Video* _video_lookup(VideoId id) {
  VideoPool* pool = &_videos;
  if (_VIDEO_INVALIDID == id.id) {
    return NULL;
  }
  int slot_index = (int)(id.id & _VIDEO_SLOT_MASK);
  if (_VIDEO_INVALID_SLOT_INDEX == slot_index) {
    return NULL;
  }
  if ((slot_index <= _VIDEO_INVALID_SLOT_INDEX) || (slot_index >= VIDEO_POOL_SIZE)) {
    return NULL;
  }
  Video* v = &pool->videos[slot_index];
  if (v->id != id.id) {
    return NULL;
  }
  return v;
}
static Video* _video_at(VideoId id) {
  Video* v = _video_lookup(id);
  assert(v);
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

VideoOpenRes video_open(const char* path, const VideoOpenParams* p) {
  VideoId vid = _video_alloc();
  Video* v = _video_at(vid);
  snprintf(v->filepath, sizeof(v->filepath), "%.*s", (int)strlen(path), path);

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
  v->vidstreamidx = -1;
  v->audiostreamidx = -1;
  for (uint32_t i = 0; i < v->fmt_ctx->nb_streams; i++) {
    enum AVMediaType type = v->fmt_ctx->streams[i]->codecpar->codec_type;
    if (type == AVMEDIA_TYPE_VIDEO && v->vidstreamidx == -1) {
      v->codec_params = v->fmt_ctx->streams[i]->codecpar;
      v->total_secs = (double)v->fmt_ctx->duration * av_q2d(AV_TIME_BASE_Q);
      v->vidstreamidx = i;
    }
    if (type == AVMEDIA_TYPE_AUDIO && v->audiostreamidx == -1) {
      v->audiostreamidx = i;
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
  // open video codec
  if (avcodec_open2(v->codec_ctx, codec, NULL) < 0) {
    err = "Failed to open codec";
    goto cleanup;
  }
  v->frame_raw = av_frame_alloc();
  v->aud_frame_raw = av_frame_alloc();
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

  // setup audio track if it exists
  if (v->audiostreamidx != -1 && !p->disable_audio) {
    res = avformat_open_input(&v->aud_fmt_ctx, path, NULL, NULL);
    if (res != 0) {
      err = "Failed to open video";
      goto cleanup;
    }
    if (avformat_find_stream_info(v->aud_fmt_ctx, NULL) < 0) {
      err = "Failed to find video stream info";
      goto cleanup;
    }
    v->aud_codec_params = v->aud_fmt_ctx->streams[v->audiostreamidx]->codecpar;
    const AVCodec* aud_codec = avcodec_find_decoder(v->aud_codec_params->codec_id);
    if (aud_codec == NULL) {
      err = "Unsupported audio codec";
      goto cleanup;
    }
    v->aud_codec_ctx = avcodec_alloc_context3(aud_codec);
    if (v->aud_codec_ctx == NULL) {
      err = "Unsupported audio codec context";
      goto cleanup;
    }
    if (avcodec_parameters_to_context(v->aud_codec_ctx, v->aud_codec_params) != 0) {
      err = "Failed to setup audio codec";
      goto cleanup;
    }
    if (avcodec_open2(v->aud_codec_ctx, aud_codec, NULL) < 0) {
      err = "Failed to open audio codec";
      goto cleanup;
    }
  }
cleanup:
  if (err != NULL) {
    video_close(vid);
    return (VideoOpenRes){.err = err};
  }
  return (VideoOpenRes){.vid = vid};
}

void video_nextframe(VideoId vid, double pos_secs, thread_mutex_t* aud_thread_mtx) {
  Video* v = _video_at(vid);
  double dt = pos_secs - v->pos_secs;
  v->pos_secs = pos_secs;
  if (v->pos_secs < 0.0) {
    v->pos_secs = 0.0;
  } else if (v->pos_secs > v->total_secs) {
    v->pos_secs = v->total_secs;
  }
  double time_base = av_q2d(av_inv_q(v->fmt_ctx->streams[v->vidstreamidx]->time_base));
  // apply seeking if required
  if (dt < 0.0 || dt > 0.01) {
    packet_queue_clear(&v->vid_queue);

    thread_mutex_lock(aud_thread_mtx);
    packet_queue_clear(&v->aud_queue);
    thread_mutex_unlock(aud_thread_mtx);

    int64_t timestamp = (int64_t)((double)v->pos_secs * time_base);
    av_seek_frame(v->fmt_ctx, v->vidstreamidx, timestamp, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(v->codec_ctx);
    if (v->aud_codec_ctx) {
      avcodec_flush_buffers(v->aud_codec_ctx);
    }
    v->next_swap_secs = 0.0f;
  }
  v->aud_playing = dt != 0.0;
  // fill the packet queues for audio and video from current position
  AVPacket packet;
  while ((!packet_queue_full(&v->vid_queue) && !packet_queue_full(&v->aud_queue)) && 
    (v->vid_queue.num_packets < 8 || v->aud_queue.num_packets < 8) &&
    av_read_frame(v->fmt_ctx, &packet) >= 0) {
    if (packet.stream_index == v->vidstreamidx) {
      packet_queue_put(&v->vid_queue, &packet);
    } else if (packet.stream_index == v->audiostreamidx) {
      thread_mutex_lock(aud_thread_mtx);
      packet_queue_put(&v->aud_queue, &packet);
      thread_mutex_unlock(aud_thread_mtx);
    }
    av_packet_unref(&packet);
  }
  // if we need to swap a video frame, decode + present the next frame
  if (v->pos_secs >= v->next_swap_secs) {
    while (true) {
      AVPacket* pkt = packet_queue_pop(&v->vid_queue);
      if (pkt == NULL) {
        break;
      }
      if (avcodec_send_packet(v->codec_ctx, pkt) < 0) {
        av_packet_unref(pkt);
        continue;
      }
      if (avcodec_receive_frame(v->codec_ctx, v->frame_raw) < 0) {
        av_packet_unref(pkt);
        av_frame_unref(v->frame_raw);
        continue;
      }
      v->next_swap_secs = (double)v->frame_raw->pts / time_base;
      if (v->next_swap_secs < v->pos_secs) {
        av_packet_unref(pkt);
        av_frame_unref(v->frame_raw);
        continue;
      }
      sws_scale(v->sws_ctx, v->frame_raw->data, v->frame_raw->linesize, 0, v->codec_params->height,
                v->frame_rgb->data, v->frame_rgb->linesize);
      sg_update_image(v->img, &(sg_image_data){.subimage[0][0] = {.ptr = v->imgbuf, .size = v->imgbuflen}});

      av_packet_unref(pkt);
      av_frame_unref(v->frame_raw);
      break;
    }
  }
}

static int video_appendaudio(AVFrame* aud_frame, float* frames, int* frame_pos, int* num_frames, int num_channels) {
  int num_samples = aud_frame->nb_samples - *frame_pos;
  const float* framesl = (float*)aud_frame->data[0];
  const float* framesr = aud_frame->channels >= 2 ? (float*)aud_frame->data[1] : framesl;
  if (num_samples > *num_frames) {
    num_samples = *num_frames;
  }
  if (num_channels >= 2) {
    for (int i = 0, j = *frame_pos * num_channels; i < num_samples; i++) {
      frames[j++] = framesl[i];
      frames[j++] = framesr[i];
    }
  } else {
    for (int i = 0, j = *frame_pos; i < num_samples; i++) {
      frames[j++] = framesl[i];
    }
  }
  *frame_pos += num_samples;
  *num_frames -= num_samples;
  return num_samples;
}

void video_getaudio_underlock(VideoId vid, float* frames, int num_frames, int num_channels, int sample_rate) {
  Video* v = _video_at(vid);
  if (v->aud_playing && v->aud_codec_ctx) {
    AVPacket* pkt = NULL;
    while (num_frames > 0) {
      if (v->aud_got_frame) {
        assert(v->aud_frame_raw->sample_rate == sample_rate);
        frames += video_appendaudio(v->aud_frame_raw, frames, &v->aud_frame_pos, &num_frames, num_channels) * 2;
        if (v->aud_frame_pos < v->aud_frame_raw->nb_samples) {
          break;
        }
        av_frame_unref(v->aud_frame_raw);
        v->aud_got_frame = false;
      }
      pkt = packet_queue_pop(&v->aud_queue);
      if (pkt == NULL) {
        break;
      }
      if (avcodec_send_packet(v->aud_codec_ctx, pkt) < 0) {
        av_packet_unref(pkt);
        continue;
      }
      if (avcodec_receive_frame(v->aud_codec_ctx, v->aud_frame_raw) >= 0) {
        v->aud_frame_pos = 0;
        v->aud_got_frame = true;
      }
      av_packet_unref(pkt);
    }
  }
  if (num_channels >= 2) {
    for (int i = 0, j = 0; i < num_frames; i++) {
      frames[j++] = 0.0f;
      frames[j++] = 0.0f;
    }
  } else {
    for (int i = 0; i < num_frames; i++) {
      frames[i] = 0.0f;
    }
  }
}

void video_close(VideoId vid) {
  Video* v = _video_lookup(vid);
  if (v == NULL) {
    return;
  }
  if (v->frame_raw) {
    av_frame_free(&v->frame_raw);
  }
  if (v->aud_frame_raw) {
    av_frame_free(&v->aud_frame_raw);
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
  if (v->codec_ctx) {
    avcodec_close(v->codec_ctx);
    avcodec_free_context(&v->codec_ctx);
  }
  if (v->aud_fmt_ctx) {
    avformat_close_input(&v->aud_fmt_ctx);
  }
  if (v->aud_codec_ctx) {
    avcodec_close(v->aud_codec_ctx);
    avcodec_free_context(&v->aud_codec_ctx);
  }
  sg_destroy_image(v->img);
  if (v->imgbuf) {
    av_free(v->imgbuf);
  }
  packet_queue_clear(&v->aud_queue);
  packet_queue_clear(&v->vid_queue);
  _video_free(vid);
}

void video_gc_clearmarks(void) {
  VideoPool* p = &_videos;
  for (int i = 0; i < p->size; i++) {
    p->videos[i].gc_marked = false;
  }
}
void video_gc_mark(VideoId vid) {
  Video* v = _video_at(vid);
  v->gc_marked = true;
}
void video_gc_sweep(void) {
  VideoPool* p = &_videos;
  for (int i = 0; i < p->size; i++) {
    if (p->videos[i].id && !p->videos[i].gc_marked) {
      video_close((VideoId){.id = p->videos[i].id});
    }
  }
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
  const char* path = _video_at(vid)->filepath;
  char* lastslash = strrchr(path, '/');
  return lastslash ? lastslash + 1 : path;
}
const char* video_filepath(VideoId vid) {
  return _video_at(vid)->filepath;
}

struct sg_image video_make_thumbnail(VideoId vid, double pos_secs, int* width, int* height) {
  Video* v = _video_at(vid);
  int64_t timestamp = (int64_t)((double)pos_secs * av_q2d(av_inv_q(v->fmt_ctx->streams[v->vidstreamidx]->time_base)));
  av_seek_frame(v->fmt_ctx, v->vidstreamidx, timestamp, AVSEEK_FLAG_BACKWARD);
  v->pos_secs = pos_secs;
  v->next_swap_secs = 0.0f;
  AVPacket packet;
  while (av_read_frame(v->fmt_ctx, &packet) >= 0) {
    if (avcodec_send_packet(v->codec_ctx, &packet) < 0) {
      av_packet_unref(&packet);
      continue;
    }
    if (avcodec_receive_frame(v->codec_ctx, v->frame_raw) >= 0) {
      int tgtheight = v->codec_params->height * *width / v->codec_params->width;
      int tgtwidth = v->codec_params->width * *height / v->codec_params->height;
      if (tgtheight < *height) {
        *height = tgtheight;
      } else {
        *width = tgtwidth;
      }
      struct SwsContext* sws_ctx =
          sws_getContext(v->codec_params->width, v->codec_params->height, v->codec_params->format, *width, *height,
                         AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
      int imgbuflen = av_image_get_buffer_size(AV_PIX_FMT_RGBA, *width, *height, 1);
      void* imgbuf = av_malloc(imgbuflen);
      AVFrame* frame_rgb = av_frame_alloc();
      av_image_fill_arrays(frame_rgb->data, frame_rgb->linesize, imgbuf, AV_PIX_FMT_RGBA, *width, *height, 1);
      sws_scale(sws_ctx, v->frame_raw->data, v->frame_raw->linesize, 0, v->codec_params->height, frame_rgb->data,
                frame_rgb->linesize);
      sg_image img = sg_make_image(&(sg_image_desc){
          .width = *width,
          .height = *height,
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
      av_packet_unref(&packet);
      return img;
    }
    av_packet_unref(&packet);
  }
  return (sg_image){0};
}
