#pragma once
// Host: the options the camera session reads. Photos on, videos off (the
// video download is not part of the host build), no PSRAM.
#define CONFIG_CAMERA_DOWNLOAD_PHOTOS 1
#define CONFIG_CAMERA_MEDIA_LIST_MAX_PAGES 8
#define CONFIG_CAMERA_BULK_ACK_FRAMES 17
#define CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S 4
#define CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S 8
