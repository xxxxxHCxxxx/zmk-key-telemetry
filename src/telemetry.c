/*
 * Streams zmk_position_state_changed / zmk_layer_state_changed as 3-byte
 * frames over a custom BLE GATT notify characteristic, for a companion
 * app that visualizes the live keymap. Only built for the split-central
 * side (see Kconfig: depends on ZMK_SPLIT_ROLE_CENTRAL), which is where
 * ZMK already re-raises the peripheral half's key events.
 */

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>

LOG_MODULE_REGISTER(zmk_key_telemetry, CONFIG_ZMK_LOG_LEVEL);

#define FRAME_TYPE_POSITION 0x01
#define FRAME_TYPE_LAYER 0x02

struct telemetry_frame {
    uint8_t type;
    uint8_t index;
    uint8_t state;
};

/* Regenerate with `uuidgen` if this module is ever forked/reused elsewhere;
 * kept distinct from ZMK Studio's own service (00000000-0196-6107-...). */
#define BT_UUID_KEY_TELEMETRY_SVC_VAL                                                            \
    BT_UUID_128_ENCODE(0xd53e3c3a, 0x511c, 0x499f, 0x9708, 0xc752305d41b4)
#define BT_UUID_KEY_TELEMETRY_CHR_VAL                                                             \
    BT_UUID_128_ENCODE(0x67f1d079, 0x5c05, 0x4d80, 0x87a1, 0x0f4a9d0c4aee)

static struct bt_uuid_128 key_telemetry_svc_uuid = BT_UUID_INIT_128(BT_UUID_KEY_TELEMETRY_SVC_VAL);
static struct bt_uuid_128 key_telemetry_chr_uuid = BT_UUID_INIT_128(BT_UUID_KEY_TELEMETRY_CHR_VAL);

static bool notify_enabled;

/* Ring buffer + deferred work: bt_gatt_notify() can block waiting on an ACL
 * TX buffer, so it must never be called directly from the ZMK event-manager
 * callback. A full queue drops the oldest frame rather than blocking - a
 * missed visual update is harmless, stalling real key input is not. */
static struct telemetry_frame queue[CONFIG_ZMK_KEY_TELEMETRY_MAX_NOTIFY_QUEUE];
static size_t queue_head;
static size_t queue_count;
static struct k_spinlock queue_lock;

static void telemetry_notify_work_cb(struct k_work *work);
static K_WORK_DEFINE(telemetry_notify_work, telemetry_notify_work_cb);

static void key_telemetry_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);
    notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

BT_GATT_SERVICE_DEFINE(key_telemetry_svc, BT_GATT_PRIMARY_SERVICE(&key_telemetry_svc_uuid),
                        BT_GATT_CHARACTERISTIC(&key_telemetry_chr_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                                                BT_GATT_PERM_NONE, NULL, NULL, NULL),
                        BT_GATT_CCC(key_telemetry_ccc_cfg_changed,
                                    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), );

static void telemetry_enqueue_frame(uint8_t type, uint8_t index, uint8_t state) {
    if (!notify_enabled) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&queue_lock);

    size_t tail = (queue_head + queue_count) % ARRAY_SIZE(queue);
    if (queue_count == ARRAY_SIZE(queue)) {
        /* Drop the oldest frame to make room. */
        queue_head = (queue_head + 1) % ARRAY_SIZE(queue);
        LOG_WRN("telemetry queue full, dropping oldest frame");
    } else {
        queue_count++;
    }
    queue[tail] = (struct telemetry_frame){.type = type, .index = index, .state = state};

    k_spin_unlock(&queue_lock, key);

    k_work_submit(&telemetry_notify_work);
}

static void telemetry_notify_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    for (;;) {
        struct telemetry_frame frame;

        k_spinlock_key_t key = k_spin_lock(&queue_lock);
        if (queue_count == 0) {
            k_spin_unlock(&queue_lock, key);
            return;
        }
        frame = queue[queue_head];
        queue_head = (queue_head + 1) % ARRAY_SIZE(queue);
        queue_count--;
        k_spin_unlock(&queue_lock, key);

        if (!notify_enabled) {
            continue;
        }

        uint8_t payload[3] = {frame.type, frame.index, frame.state};
        int rc = bt_gatt_notify(NULL, &key_telemetry_svc.attrs[1], payload, sizeof(payload));
        if (rc == -ENOMEM) {
            /* Transient backpressure (e.g. connection interval hasn't
             * elapsed yet). Stop draining for now; the next enqueued
             * frame's work submission will retry. */
            LOG_WRN("bt_gatt_notify backpressure, deferring remaining frames");
            return;
        } else if (rc < 0) {
            LOG_DBG("bt_gatt_notify failed: %d", rc);
        }
    }
}

static int telemetry_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    telemetry_enqueue_frame(FRAME_TYPE_POSITION, (uint8_t)ev->position, ev->state ? 1 : 0);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_key_telemetry_position, telemetry_position_listener);
ZMK_SUBSCRIPTION(zmk_key_telemetry_position, zmk_position_state_changed);

static int telemetry_layer_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    telemetry_enqueue_frame(FRAME_TYPE_LAYER, ev->layer, ev->state ? 1 : 0);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_key_telemetry_layer, telemetry_layer_listener);
ZMK_SUBSCRIPTION(zmk_key_telemetry_layer, zmk_layer_state_changed);
