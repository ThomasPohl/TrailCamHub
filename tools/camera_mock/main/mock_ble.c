#include "mock_ble.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define SERVICE_UUID 0x8801
#define CONTROL_UUID 0x0002
#define RESPONSE_UUID 0x0003
#define MAX_WRITE_SIZE 64
#define MAX_ADV_SIZE 31

static const char *TAG = "mock_ble";

static uint8_t s_adv[MAX_ADV_SIZE];
static size_t s_adv_size;
static mock_ble_write_cb_t s_on_write;
static uint8_t s_own_addr_type;
static uint16_t s_response_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_subscribed;

static void start_advertising(void);

static int access_cb(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;  // reads of 0x0003 return nothing
    }
    uint8_t data[MAX_WRITE_SIZE];
    uint16_t size = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), &size) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (s_on_write != NULL) {
        s_on_write(conn_handle, data, size);
    }
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(SERVICE_UUID),
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    // Write (with response) only, as on the camera.
                    .uuid = BLE_UUID16_DECLARE(CONTROL_UUID),
                    .access_cb = access_cb,
                    .flags = BLE_GATT_CHR_F_WRITE,
                },
                {
                    .uuid = BLE_UUID16_DECLARE(RESPONSE_UUID),
                    .access_cb = access_cb,
                    .flags = BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_response_handle,
                },
                {0},
            },
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_subscribed = false;
            ESP_LOGI(TAG, "Hub connected (handle %u)", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Hub disconnected (reason 0x%x)",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_response_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Notifications on 0x0003 %s",
                     s_subscribed ? "enabled" : "disabled");
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "ATT MTU %u", event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
        return;
    }
    int rc = ble_gap_adv_set_data(s_adv, (int)s_adv_size);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not set the advertising data: %d", rc);
        return;
    }
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not start advertising: %d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "No usable BLE address: %d", rc);
        return;
    }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "Advertising as %02x:%02x:%02x:%02x:%02x:%02x", addr[5],
             addr[4], addr[3], addr[2], addr[1], addr[0]);
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset: %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t mock_ble_start(const uint8_t *adv, size_t adv_size,
                         const char *device_name, mock_ble_write_cb_t on_write)
{
    if (adv == NULL || adv_size > sizeof(s_adv)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_adv, adv, adv_size);
    s_adv_size = adv_size;
    s_on_write = on_write;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set(device_name);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        return ESP_FAIL;
    }
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t mock_ble_notify(uint16_t conn_handle, const uint8_t *data,
                          size_t size)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)size);
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Consumes `om` also on failure.
    int rc = ble_gatts_notify_custom(conn_handle, s_response_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Notification failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool mock_ble_connected(bool *subscribed)
{
    if (subscribed != NULL) {
        *subscribed = s_subscribed;
    }
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
