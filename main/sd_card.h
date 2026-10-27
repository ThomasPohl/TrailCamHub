#pragma once

#include "esp_err.h"

#define SD_CARD_MOUNT_POINT "/sdcard"

/*
 * Mounts the FAT file system of the SD card at SD_CARD_MOUNT_POINT using the
 * SDMMC peripheral in 1-bit mode. The card is never formatted automatically.
 */
esp_err_t sd_card_mount(void);
