cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)

project(filmsaw)

add_executable(filmsaw
	src/main.c src/ui.h src/ui.c src/video.h src/video.c src/video_clips.h src/video_clips.c
	src/debuglog.h
	src/3rdparty/dirent.h src/3rdparty/json.h
	src/3rdparty/sokol/sokol_app.h src/3rdparty/sokol/sokol_gfx.h src/3rdparty/sokol/sokol.c 
	src/3rdparty/stb/stb_image.h src/3rdparty/stb/stb_image.c
	src/3rdparty/fontstash/fontstash.h src/3rdparty/fontstash/stb_truetype.h

	data/fonts/vera.c data/icons.c Resource.rc)
add_compile_definitions(filmsaw SOKOL_D3D11 _CRT_SECURE_NO_WARNINGS)
target_include_directories(filmsaw PUBLIC src/3rdparty src/3rdparty/ffmpeg/include)
set_property(TARGET filmsaw PROPERTY C_STANDARD 17)
add_custom_command(TARGET filmsaw POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:filmsaw> $<TARGET_FILE_DIR:filmsaw>
  COMMAND_EXPAND_LISTS
)

add_library(avcodec SHARED IMPORTED)
set_property(TARGET avcodec PROPERTY IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/src/3rdparty/ffmpeg/bin/avcodec-59.dll)
set_property(TARGET avcodec PROPERTY IMPORTED_IMPLIB ${PROJECT_SOURCE_DIR}/src/3rdparty/ffmpeg/lib/avcodec.lib)

add_library(avformat SHARED IMPORTED)
set_property(TARGET avformat PROPERTY IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/src/3rdparty/ffmpeg/bin/avformat-59.dll)
set_property(TARGET avformat PROPERTY IMPORTED_IMPLIB ${PROJECT_SOURCE_DIR}/src/3rdparty/ffmpeg/lib/avformat.lib)

add_library(swscale SHARED IMPORTED)
set_property(TARGET swscale PROPERTY IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/src/3rdparty/ffmpeg/bin/swscale-6.dll)
set_property(TARGET swscale PROPERTY IMPORTED_IMPLIB ${PROJECT_SOURCE_DIR}/src/3rdparty/ffmpeg/lib/swscale.lib)

add_library(swresample SHARED IMPORTED)
set_property(TARGET swresample PROPERTY IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/src/3rdparty/ffmpeg/bin/swresample-4.dll)
set_property(TARGET swresample PROPERTY IMPORTED_IMPLIB ${PROJECT_SOURCE_DIR}/src/3rdparty/ffmpeg/lib/swresample.lib)

add_library(avutil SHARED IMPORTED)
set_property(TARGET avutil PROPERTY IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/src/3rdparty/ffmpeg/bin/avutil-57.dll)
set_property(TARGET avutil PROPERTY IMPORTED_IMPLIB ${PROJECT_SOURCE_DIR}/src/3rdparty/ffmpeg/lib/avutil.lib)

target_link_libraries(filmsaw avcodec avformat swscale swresample avutil)
