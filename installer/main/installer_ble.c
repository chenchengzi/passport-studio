// installer/main/installer_ble.c
#include "installer_ble.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "inst_ble";

// UUIDs from inst_proto.h in NimBLE's little-endian byte order; byte 12 selects
// the characteristic.
#define INST_UUID(n) BLE_UUID128_INIT(0x8A, 0x91, 0xF6, 0xE0, 0x8D, 0x26, 0x0F, 0x89, \
                                      0xC6, 0x47, 0x5A, 0xDD, (n), 0x6C, 0xDE, 0xCD)
static const ble_uuid128_t UUID_SERVICE = INST_UUID(0x00);
static const ble_uuid128_t UUID_INFO = INST_UUID(0x01);
static const ble_uuid128_t UUID_CONTROL = INST_UUID(0x02);
static const ble_uuid128_t UUID_DATA = INST_UUID(0x03);
static const ble_uuid128_t UUID_STATUS = INST_UUID(0x04);
static const ble_uuid128_t UUID_WIFI = INST_UUID(0x05);

static installer_ble_callbacks_t s_cb;
static uint8_t s_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_handle;
static volatile bool s_status_subscribed;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_status[INST_STATUS_LEN];

static int gap_event(struct ble_gap_event *event, void *arg);

static int access_info(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    uint8_t buf[192];
    size_t len = s_cb.read_info(buf, sizeof(buf));
    return os_mbuf_append(ctxt->om, buf, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_wifi(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    uint8_t buf[192];
    size_t len = s_cb.read_wifi(buf, sizeof(buf));
    return os_mbuf_append(ctxt->om, buf, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_status(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    uint8_t copy[INST_STATUS_LEN];
    taskENTER_CRITICAL(&s_status_lock);
    memcpy(copy, s_status, sizeof(copy));
    taskEXIT_CRITICAL(&s_status_lock);
    return os_mbuf_append(ctxt->om, copy, sizeof(copy)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_write(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    static uint8_t buf[BLE_ATT_ATTR_MAX_LEN];   // host task only
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0 || len == 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    return s_cb.on_packet(buf, len) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

#define SECURE_READ  (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)
#define SECURE_WRITE (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SERVICE.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_INFO.u, .access_cb = access_info, .flags = SECURE_READ },
            { .uuid = &UUID_CONTROL.u, .access_cb = access_write, .flags = SECURE_WRITE },
            { .uuid = &UUID_DATA.u, .access_cb = access_write,
              .flags = SECURE_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &UUID_STATUS.u, .access_cb = access_status, .val_handle = &s_status_handle,
              .flags = SECURE_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &UUID_WIFI.u, .access_cb = access_wifi, .flags = SECURE_READ },
            { 0 },
        },
    },
    { 0 },
};

static void advertise(void)
{
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = &UUID_SERVICE;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc == 0) {
        struct ble_hs_adv_fields rsp = {0};
        const char *name = ble_svc_gap_device_name();
        rsp.name = (const uint8_t *)name;
        rsp.name_len = strlen(name);
        rsp.name_is_complete = 1;
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }
    if (rc == 0) {
        struct ble_gap_adv_params params = {
            .conn_mode = BLE_GAP_CONN_MODE_UND,
            .disc_mode = BLE_GAP_DISC_MODE_GEN,
        };
        rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "advertising failed: %d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            break;
        }
        s_conn = event->connect.conn_handle;
        s_status_subscribed = false;
        // Throughput hints; the phone may refuse them.
        ble_gap_set_data_len(s_conn, 251, 2120);
        ble_gap_set_prefered_le_phy(s_conn, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
        s_cb.on_connection(true);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_status_subscribed = false;
        s_cb.on_connection(false);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = {
                .action = BLE_SM_IOACT_DISP,
                .passkey = esp_random() % 1000000,
            };
            s_cb.on_passkey(io.passkey);
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        s_cb.on_secured(event->enc_change.status == 0);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // No bonds are kept, but drop any stale one so the phone can pair again.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_status_handle) {
            s_status_subscribed = event->subscribe.cur_notify;
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable Bluetooth address");
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t installer_ble_start(const char *device_name, const installer_ble_callbacks_t *callbacks)
{
    s_cb = *callbacks;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    // Passkey shown on the device screen, LE Secure Connections, no bonding:
    // every connection pairs again, and nothing is written to the play's NVS.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(SERVICES);
    if (rc == 0) rc = ble_gatts_add_svcs(SERVICES);
    if (rc == 0) rc = ble_svc_gap_device_name_set(device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        return ESP_FAIL;
    }
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void installer_ble_set_status(const uint8_t status[INST_STATUS_LEN])
{
    taskENTER_CRITICAL(&s_status_lock);
    memcpy(s_status, status, INST_STATUS_LEN);
    taskEXIT_CRITICAL(&s_status_lock);
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_status_subscribed) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, INST_STATUS_LEN);
    if (om) ble_gatts_notify_custom(conn, s_status_handle, om);
}
