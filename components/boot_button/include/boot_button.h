#pragma once

#include "esp_err.h"

/*
 * The board's BOOT button as a front end (#224): a short press requests a
 * camera search, a long press stops it. The component knows nothing about the
 * camera workflow; the application passes the actions as callbacks.
 */

typedef struct {
    void (*on_search)(void);
    void (*on_stop)(void);
} boot_button_callbacks_t;

/* Starts the task that polls the button. */
esp_err_t boot_button_start(const boot_button_callbacks_t *callbacks);
