#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
  uint32_t id;
} VideoId;

#define VIDEO_POOL_SIZE (1024)

void videopool_init();

typedef union thread_mutex_t thread_mutex_t;

typedef struct VideoOpenParams {
  int aud_num_channels, aud_sample_rate;
  bool disable_audio;
} VideoOpenParams;
typedef struct {
  VideoId vid;
  const char* err;
} VideoOpenRes;
VideoOpenRes video_open(const char* path, const VideoOpenParams* p);
void video_nextframe(VideoId vid, double pos_secs, thread_mutex_t* aud_thread_mtx); // locks aud_thread_mtx
void video_getaudio_underlock(VideoId vid, float* frames, int num_frames);          // assumes aud_thread_mtx is locked
void video_close(VideoId vid);
double video_total_secs(VideoId vid);
double video_pos_secs(VideoId vid);
int video_width(VideoId vid);
int video_height(VideoId vid);
struct sg_image video_image(VideoId vid);
const char* video_filename(VideoId vid);
const char* video_filepath(VideoId vid);

// makes a new image thumbnail. the caller takes ownership of the sg_image if successful.
struct sg_image video_make_thumbnail(VideoId vid, double pos_secs, int* width, int* height);
