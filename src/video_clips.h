#pragma once
#include <sokol/sokol_gfx.h>
#include "video.h"

typedef struct {
  double pos, clipstart, clipend;
  int track;
  sg_image thumbnail;
  VideoId vid;
} VideoClip;

typedef struct {
  VideoClip* clips;
  int num, cap;
} VideoClips;

void videoclips_push(VideoClips* l, VideoClip c);
const char* videoclips_save(const char* path, const VideoClips* clips);
const char* videoclips_load(const char* path, VideoClips* clips, const struct VideoOpenParams* p);
void videoclips_free(VideoClips* l);
