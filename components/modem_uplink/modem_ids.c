/*
 * USB ids of the modems the hub can drive: vendor/product, the interface
 * that carries PPP (and AT commands, as long as no data call runs) and an
 * optional separate AT interface (-1: none). From the esp-iot-solution
 * usb_cdc_4g_module example.
 */

#include "modem_ids.h"

#include <stddef.h>

const usb_modem_id_t trailcamhub_modem_ids[] = {
    {
        .match_id =
            {
                .match_flags = USB_DEVICE_ID_MATCH_VID_PID,
                .idVendor = 0x1E0E,
                .idProduct = 0x9011,
            },
        .modem_itf_num = 5,
        .at_itf_num = -1,
        .name = "SIMCOM A7670E",
    },
    {.name = NULL},  // end of the list (match_flags 0)
};
