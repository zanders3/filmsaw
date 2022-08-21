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

  struct SwrContext* swr_ctx;
  AVCodecParameters* aud_codec_params;
  AVCodecContext* aud_codec_ctx;
  int aud_num_channels, aud_sample_rate;
  int audiostreamidx, audbuflinesize;
  uint8_t** audbuf;

  char filepath[_MAX_PATH + 1];

  double pos_secs, next_swap_secs, total_secs;
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
      v->aud_codec_params = v->fmt_ctx->streams[i]->codecpar;
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
  if (v->aud_codec_params && !p->disable_audio) {
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
    v->swr_ctx = swr_alloc();
    av_opt_set_chlayout(v->swr_ctx, "in_chlayout", &v->aud_codec_ctx->ch_layout, 0);
    av_opt_set_int(v->swr_ctx, "in_sample_rate", v->aud_codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(v->swr_ctx, "in_sample_fmt", v->aud_codec_ctx->sample_fmt, 0);

    v->aud_sample_rate = p->aud_sample_rate;
    v->aud_num_channels = 2; // p->aud_num_channels;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    av_opt_set_chlayout(v->swr_ctx, "out_chlayout", v->aud_num_channels == 2 ? &stereo : &mono, 0);
    av_opt_set_int(v->swr_ctx, "out_sample_rate", (int64_t)v->aud_sample_rate, 0);
    av_opt_set_sample_fmt(v->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);

    int ret = swr_init(v->swr_ctx);
    if (ret < 0) {
      err = "Failed to setup audio rescale";
      goto cleanup;
    }
    if (av_samples_alloc_array_and_samples(&v->audbuf, &v->audbuflinesize, v->aud_num_channels, v->aud_sample_rate,
                                           AV_SAMPLE_FMT_FLTP, 0) < 0) {
      err = "Failed to alloc audio buffer";
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

void video_nextframe(VideoId vid, double pos_secs) {
  Video* v = _video_at(vid);
  double dt = pos_secs - v->pos_secs;
  v->pos_secs = pos_secs;
  if (v->pos_secs < 0.0) {
    v->pos_secs = 0.0;
  } else if (v->pos_secs > v->total_secs) {
    v->pos_secs = v->total_secs;
  }
  if (dt < 0.0 || dt > 0.01) {
    int64_t timestamp =
        (int64_t)((double)v->pos_secs * av_q2d(av_inv_q(v->fmt_ctx->streams[v->vidstreamidx]->time_base)));
    av_seek_frame(v->fmt_ctx, v->vidstreamidx, timestamp, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(v->codec_ctx);
    if (v->aud_codec_ctx) {
      avcodec_flush_buffers(v->aud_codec_ctx);
    }
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
    return;
  }
  if (v->next_swap_secs > v->pos_secs) {
    return;
  }
  bool did_image = false;
  AVPacket packet;
  while (av_read_frame(v->fmt_ctx, &packet) >= 0) {
    if (packet.stream_index == v->vidstreamidx) {
      if (avcodec_send_packet(v->codec_ctx, &packet) < 0) {
        av_packet_unref(&packet);
        continue;
      }
      if (avcodec_receive_frame(v->codec_ctx, v->frame_raw) >= 0) {
        sws_scale(v->sws_ctx, v->frame_raw->data, v->frame_raw->linesize, 0, v->codec_params->height,
                  v->frame_rgb->data, v->frame_rgb->linesize);
        if (!did_image) {
          sg_update_image(v->img, &(sg_image_data){.subimage[0][0] = {.ptr = v->imgbuf, .size = v->imgbuflen}});
          did_image = true;
        }
        v->next_swap_secs = (double)v->frame_raw->pts * av_q2d(v->fmt_ctx->streams[v->vidstreamidx]->time_base);
        av_frame_unref(v->frame_raw);
      }
    } else if (packet.stream_index == v->audiostreamidx) {
      if (avcodec_send_packet(v->aud_codec_ctx, &packet) < 0) {
        av_packet_unref(&packet);
        continue;
      }
      if (avcodec_receive_frame(v->aud_codec_ctx, v->frame_raw) >= 0) {
        int numsamples =
            av_rescale_rnd(v->frame_raw->nb_samples, v->aud_sample_rate, v->frame_raw->sample_rate, AV_ROUND_UP);
        int ret = swr_convert(v->swr_ctx, v->audbuf, numsamples, (const uint8_t**)v->frame_raw->data,
                              v->frame_raw->nb_samples);
        if (ret >= 0) {
          int dst_bufsize =
              av_samples_get_buffer_size(&v->audbuflinesize, v->aud_num_channels, ret, AV_SAMPLE_FMT_FLTP, 1);
          const float* framesl = (float*)v->audbuf[0];
          const float* framesr = (float*)v->audbuf[1];
          int num_frames = dst_bufsize / (sizeof(float) * 2);
          float* frames_tmp = malloc(dst_bufsize);
          for (int i = 0; i < num_frames; i++) {
            frames_tmp[i * 2] = framesl[i];
            frames_tmp[(i * 2) + 1] = framesr[i];
          }
          saudio_push(frames_tmp, num_frames);
          free(frames_tmp);
        }
      }
      av_frame_unref(v->frame_raw);
      av_packet_unref(&packet);
      continue;
    }
    av_packet_unref(&packet);
    break;
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
  if (v->audbuf) {
    av_free(v->audbuf);
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
  const char* path = _video_at(vid)->filepath;
  char* lastslash = strrchr(path, '/');
  return lastslash ? lastslash + 1 : path;
}
const char* video_filepath(VideoId vid) {
  return _video_at(vid)->filepath;
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
