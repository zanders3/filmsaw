#pragma once
#include <stdint.h>

typedef struct {
  uint32_t id;
} VideoId;

#define VIDEO_POOL_SIZE (1024)

void videopool_init();

typedef struct {
  VideoId vid;
  const char* err;
} VideoOpenRes;
VideoOpenRes video_open(const char* path);
void video_seek(VideoId vid, double pos_secs);
void video_nextframe(VideoId vid, double dt);
void video_close(VideoId vid);
double video_total_secs(VideoId vid);
double video_pos_secs(VideoId vid);
int video_width(VideoId vid);
int video_height(VideoId vid);
struct sg_image video_image(VideoId vid);
const char* video_filename(VideoId vid);

// makes a new image thumbnail. the caller takes ownership of the sg_image if successful.
struct sg_image video_make_thumbnail(VideoId vid, double pos_secs, int width, int height);
