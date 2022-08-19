#include "video_clips.h"
#include <stdio.h>
#include <dirent.h>
#include "json.h"
#include <assert.h>
#include "debuglog.h"

void videoclips_push(VideoClips* l, VideoClip c) {
  if (l->num + 1 >= l->cap) {
    l->cap = l->cap ? l->cap * 2 : 16;
    void* newblock = realloc(l->clips, l->cap * sizeof(VideoClip));
    assert(newblock);
    l->clips = (VideoClip*)newblock;
  }
  l->clips[l->num++] = c;
}

void videoclips_free(VideoClips* l) {
  for (int i = 0; i < l->num; i++) {
    VideoClip* clip = &l->clips[i];
    sg_destroy_image(clip->thumbnail);
    video_close(clip->vid);
  }
  free(l->clips);
  *l = (VideoClips){0};
}

const char* videoclips_save(const char* path, const VideoClips* clips) {
  FILE* f = fopen(path, "wb");
  if (f == NULL) {
    return "failed to open file";
  }
  fprintf(f, "{\"clips\":[\r\n");
  for (int i = 0; i < clips->num; i++) {
    const VideoClip* clip = &clips->clips[i];
    const char* video_path = video_filepath(clip->vid);
    char escaped_path[PATH_MAX];
    int escaped_path_len = 0;
    while (*video_path != '\0' && escaped_path_len + 1 < PATH_MAX) {
      escaped_path[escaped_path_len++] = *video_path;
      if (*video_path == '\\') {
        escaped_path[escaped_path_len++] = '\\';
      }
      video_path++;
    }
    escaped_path[escaped_path_len] = '\0';
    fprintf(f, "{\"pos\":%f,\"clipstart\":%f,\"clipend\":%f,\"track\":%d,\"path\":\"%s\"}%s\r\n", clip->pos,
            clip->clipstart, clip->clipend, clip->track, escaped_path, i + 1 < clips->num ? "," : "");
  }
  fprintf(f, "]}\r\n");
  fclose(f);
  return NULL;
}
static double json_value_as_double(struct json_value_s* el) {
  struct json_number_s* num = json_value_as_number(el);
  if (num) {
    const char* endptr = num->number + num->number_size;
    return strtod(num->number, (char**)&endptr);
  } else {
    return 0.0;
  }
}

const char* videoclips_load(const char* path, VideoClips* clips) {
  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    return "failed to open file";
  }
  fseek(f, 0, SEEK_END);
  size_t flen = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* buf = malloc(flen);
  assert(buf);
  size_t len = fread(buf, 1, flen, f);
  fclose(f);
  if (len != flen) {
    return "failed to read file";
  }

  struct json_value_s* root = json_parse(buf, len);
  if (root) {
    struct json_object_s* file = json_value_as_object(root);
    if (file) {
      for (struct json_object_element_s* entry = file->start; entry; entry = entry->next) {
        if (strcmp(entry->name->string, "clips") == 0) {
          struct json_array_s* clips_arr = json_value_as_array(entry->value);
          if (clips_arr) {
            for (struct json_array_element_s* clipentry = clips_arr->start; clipentry; clipentry = clipentry->next) {
              struct json_object_s* clip = json_value_as_object(clipentry->value);
              if (clip == NULL) {
                continue;
              }
              VideoClip parsedclip = (VideoClip){0};
              for (struct json_object_element_s* clipobjentry = clip->start; clipobjentry;
                   clipobjentry = clipobjentry->next) {
                if (strcmp(clipobjentry->name->string, "pos") == 0) {
                  parsedclip.pos = json_value_as_double(clipobjentry->value);
                } else if (strcmp(clipobjentry->name->string, "clipstart") == 0) {
                  parsedclip.clipstart = json_value_as_double(clipobjentry->value);
                } else if (strcmp(clipobjentry->name->string, "clipend") == 0) {
                  parsedclip.clipend = json_value_as_double(clipobjentry->value);
                } else if (strcmp(clipobjentry->name->string, "track") == 0) {
                  parsedclip.track = (int)json_value_as_double(clipobjentry->value);
                } else if (strcmp(clipobjentry->name->string, "path") == 0) {
                  const char* video_path = json_value_as_string(clipobjentry->value)->string;
                  VideoOpenRes res = video_open(video_path);
                  if (res.err) {
                    DebugLog("failed to open %s: %s\n", path, res.err);
                    free(root);
                    return "failed to open video file";
                  } else {
                    parsedclip.vid = res.vid;
                  }
                }
              }
              if (parsedclip.vid.id) {
                parsedclip.thumbnail = video_make_thumbnail(parsedclip.vid, parsedclip.clipstart, 100, 100);
                videoclips_push(clips, parsedclip);
              }
            }
          }
          break;
        }
      }
    }
  }
  free(root);
  return NULL;
}
