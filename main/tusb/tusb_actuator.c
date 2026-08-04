#include "tusb_actuator.h"
#include "tusb_device.h"
#include "keycodes.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "tusb.h"

static const char *TAG = "tusb_actuator";

#define HID_MAX_KEYS        6
#define JIGGLE_DELAY_MS     10
#define JIGGLE_DELTA        1
#define ABS_PTR_MAX         32767
#define ABS_MOUSE_REPORT_LEN 6  // buttons(1) + X(16 LE) + Y(16 LE) + wheel(1)
#define KEYBOARD_REPORT_LEN 8  // modifier(1) + reserved(1) + keycodes(6)
#define HID_RETRY_MS        5

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static inline uint16_t to_hid_x(const tusb_actuator_t *act, uint16_t x)
{
    uint16_t denom = act->screen_width - 1;
    if (denom == 0) return 0;
    return (uint16_t)(((uint32_t)x * ABS_PTR_MAX) / denom);
}

static inline uint16_t to_hid_y(const tusb_actuator_t *act, uint16_t y)
{
    uint16_t denom = act->screen_height - 1;
    if (denom == 0) return 0;
    return (uint16_t)(((uint32_t)y * ABS_PTR_MAX) / denom);
}

static inline uint16_t clamp_u16(int32_t v, uint16_t max)
{
    if (v < 0) return 0;
    if ((uint32_t)v > max) return max;
    return (uint16_t)v;
}

static int8_t clamp_int8(int16_t val)
{
    if (val > INT8_MAX) return INT8_MAX;
    if (val < INT8_MIN) return INT8_MIN;
    return (int8_t)val;
}

/* ------------------------------------------------------------------ */
/* HID report sending                                                  */
/* ------------------------------------------------------------------ */

/* Send keyboard report - block until accepted via callback wake */
static void send_kb_report(tusb_actuator_t *act, uint8_t *report)
{
    uint32_t ul_notif;

    tusb_set_pending_send(true);
    (void)tusb_try_remote_wakeup();

    while (!tud_hid_report(DF_HID_KEYBOARD, report, KEYBOARD_REPORT_LEN)) {
        (void)xTaskNotifyWait(0, 0, &ul_notif, portMAX_DELAY);
    }
    tusb_set_pending_send(false);
}

/* Send absolute mouse report - block until accepted via callback wake */
static void send_abs_mouse_retry(tusb_actuator_t *act)
{
    uint16_t hx = to_hid_x(act, act->target_x);
    uint16_t hy = to_hid_y(act, act->target_y);

    bool important = (act->mouse_buttons != act->last_sent_buttons);

    uint8_t report[ABS_MOUSE_REPORT_LEN] = {
        act->mouse_buttons,
        (uint8_t)(hx & 0xFF),        (uint8_t)(hx >> 8),
        (uint8_t)(hy & 0xFF),        (uint8_t)(hy >> 8),
        (uint8_t)act->mouse_wheel
    };

    uint32_t ul_notif;
    tusb_set_pending_send(true);
    (void)tusb_try_remote_wakeup();
    do {
        if (tud_hid_report(DF_HID_ABS_MOUSE, report, ABS_MOUSE_REPORT_LEN)) {
            act->last_sent_hx      = hx;
            act->last_sent_hy      = hy;
            act->last_sent_buttons = act->mouse_buttons;
            act->mouse_wheel       = 0;
            tusb_set_pending_send(false);
            return;
        }
        if (!important) {
            tusb_set_pending_send(false);
            return;
        }
        (void)xTaskNotifyWait(0, 0, &ul_notif, portMAX_DELAY);
    } while (true);
}

/* Fire-and-forget absolute mouse movement (no retry). */
static void send_abs_mouse_once(tusb_actuator_t *act)
{
    uint16_t hx = to_hid_x(act, act->target_x);
    uint16_t hy = to_hid_y(act, act->target_y);

    if (hx == act->last_sent_hx && hy == act->last_sent_hy &&
        act->mouse_buttons == act->last_sent_buttons &&
        act->mouse_wheel == 0) {
        return;
    }

    uint8_t report[ABS_MOUSE_REPORT_LEN] = {
        act->mouse_buttons,
        (uint8_t)(hx & 0xFF),        (uint8_t)(hx >> 8),
        (uint8_t)(hy & 0xFF),        (uint8_t)(hy >> 8),
        (uint8_t)act->mouse_wheel
    };

    if (tud_hid_report(DF_HID_ABS_MOUSE, report, ABS_MOUSE_REPORT_LEN)) {
        act->last_sent_hx      = hx;
        act->last_sent_hy      = hy;
        act->last_sent_buttons = act->mouse_buttons;
        act->mouse_wheel       = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Button mapping                                                      */
/* ------------------------------------------------------------------ */

static uint8_t barrier_button_to_hid(int8_t button)
{
    switch (button) {
        case 1: return 0x01;  // left
        case 2: return 0x04;  // middle
        case 3: return 0x02;  // right
        default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Queue-based API (called by network task)                            */
/* ------------------------------------------------------------------ */

/* Dedup window: reject identical event only if within this many ms */
#define ACT_DEDUP_MS  30

/* Dump queue contents for debugging - drains queue, prints, does NOT restore */
static void _dump_queue(tusb_actuator_t *act, const char *label)
{
    act_cmd_t item;
    UBaseType_t depth = uxQueueMessagesWaiting(act->cmd_queue);
    ESP_LOGW(TAG, "=== QUEUE DUMP (%s) items=%ld ===", label, (long)depth);
    for (UBaseType_t i = 0; i < depth; i++) {
        if (xQueueReceive(act->cmd_queue, &item, 0) == pdTRUE) {
            switch (item.type) {
                case ACT_CMD_KEY_DOWN:       ESP_LOGW(TAG, "  [%ld] KEY_DOWN   key=0x%04x mask=0x%04x", (long)i, item.d.k.key, item.d.k.mask); break;
                case ACT_CMD_KEY_UP:         ESP_LOGW(TAG, "  [%ld] KEY_UP     key=0x%04x mask=0x%04x", (long)i, item.d.k.key, item.d.k.mask); break;
                case ACT_CMD_KEY_REPEAT:     ESP_LOGW(TAG, "  [%ld] KEY_REPEAT", (long)i); break;
                case ACT_CMD_MOUSE_DOWN:     ESP_LOGW(TAG, "  [%ld] MOUSE_DOWN btn=%d", (long)i, item.d.btn.id); break;
                case ACT_CMD_MOUSE_UP:       ESP_LOGW(TAG, "  [%ld] MOUSE_UP   btn=%d", (long)i, item.d.btn.id); break;
                case ACT_CMD_MOUSE_WHEEL:    ESP_LOGW(TAG, "  [%ld] MOUSE_WHEEL dx=%d dy=%d", (long)i, item.d.wheel.dx, item.d.wheel.dy); break;
                case ACT_CMD_ENTER:          ESP_LOGW(TAG, "  [%ld] ENTER      x=%u y=%u", (long)i, item.d.enter.x, item.d.enter.y); break;
                case ACT_CMD_LEAVE:          ESP_LOGW(TAG, "  [%ld] LEAVE", (long)i); break;
                case ACT_CMD_JIGGLE:         ESP_LOGW(TAG, "  [%ld] JIGGLE", (long)i); break;
                case ACT_CMD_CONNECTED:      ESP_LOGW(TAG, "  [%ld] CONNECTED", (long)i); break;
                case ACT_CMD_DISCONNECTED:   ESP_LOGW(TAG, "  [%ld] DISCONNECTED", (long)i); break;
                case ACT_CMD_SET_SCREEN_SIZE:ESP_LOGW(TAG, "  [%ld] SCREEN_SIZE %ux%u", (long)i, item.d.screen_size.w, item.d.screen_size.h); break;
                case ACT_CMD_SET_SCALING:    ESP_LOGW(TAG, "  [%ld] SCALING %u", (long)i, item.d.scale.scaling); break;
            }
        }
    }
    ESP_LOGW(TAG, "=== END DUMP ===");
}

static esp_err_t _q(tusb_actuator_t *act, act_cmd_t cmd)
{
    if (xQueueSend(act->cmd_queue, &cmd, pdMS_TO_TICKS(10)) != pdTRUE) {
        _dump_queue(act, "QUEUE_FULL");
        return ESP_ERR_TIMEOUT;
    }
    if (act->hid_task != NULL) xTaskNotifyGive(act->hid_task);
    return ESP_OK;
}

esp_err_t actuator_queue_key_down(tusb_actuator_t *act, uint16_t key, uint16_t mask, uint16_t button)
{
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    if (act->last_queued_kb_type == ACT_CMD_KEY_DOWN && act->last_queued_kb_key == key && act->last_queued_kb_mask == mask &&
        (now - act->last_queued_kb_time) < ACT_DEDUP_MS) return ESP_OK;
    act->last_queued_kb_key = key;
    act->last_queued_kb_mask = mask;
    act->last_queued_kb_type = ACT_CMD_KEY_DOWN;
    act->last_queued_kb_time = now;
    act_cmd_t cmd = { .type = ACT_CMD_KEY_DOWN, .d.k = { key, mask, button } };
    return _q(act, cmd);
}

esp_err_t actuator_queue_key_up(tusb_actuator_t *act, uint16_t key, uint16_t mask, uint16_t button)
{
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    if (act->last_queued_kb_type == ACT_CMD_KEY_UP && act->last_queued_kb_key == key && act->last_queued_kb_mask == mask &&
        (now - act->last_queued_kb_time) < ACT_DEDUP_MS) return ESP_OK;
    act->last_queued_kb_key = key;
    act->last_queued_kb_mask = mask;
    act->last_queued_kb_type = ACT_CMD_KEY_UP;
    act->last_queued_kb_time = now;
    act_cmd_t cmd = { .type = ACT_CMD_KEY_UP, .d.k = { key, mask, button } };
    return _q(act, cmd);
}

esp_err_t actuator_queue_key_repeat(tusb_actuator_t *act, uint16_t key, uint16_t mask, uint16_t button, uint16_t count)
{
    (void)key; (void)mask; (void)button; (void)count;
    /* never queue repeats - host OS handles native key repeat */
    return ESP_OK;
}

esp_err_t actuator_queue_mouse_down(tusb_actuator_t *act, int8_t button)
{
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    if (act->last_queued_btn_type == ACT_CMD_MOUSE_DOWN && act->last_queued_btn_id == button &&
        (now - act->last_queued_btn_time) < ACT_DEDUP_MS) return ESP_OK;
    act->last_queued_btn_id = button;
    act->last_queued_btn_type = ACT_CMD_MOUSE_DOWN;
    act->last_queued_btn_time = now;
    act_cmd_t cmd = { .type = ACT_CMD_MOUSE_DOWN, .d.btn = { button } };
    return _q(act, cmd);
}

esp_err_t actuator_queue_mouse_up(tusb_actuator_t *act, int8_t button)
{
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    if (act->last_queued_btn_type == ACT_CMD_MOUSE_UP && act->last_queued_btn_id == button &&
        (now - act->last_queued_btn_time) < ACT_DEDUP_MS) return ESP_OK;
    act->last_queued_btn_id = button;
    act->last_queued_btn_type = ACT_CMD_MOUSE_UP;
    act->last_queued_btn_time = now;
    act_cmd_t cmd = { .type = ACT_CMD_MOUSE_UP, .d.btn = { button } };
    return _q(act, cmd);
}

esp_err_t actuator_queue_mouse_wheel(tusb_actuator_t *act, int16_t dx, int16_t dy)
{
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    /* Time-only dedup - prevents scroll flood regardless of dy value */
    if ((now - act->last_queued_wheel_time) < ACT_DEDUP_MS) return ESP_OK;
    act->last_queued_wheel_time = now;
    act_cmd_t cmd = { .type = ACT_CMD_MOUSE_WHEEL, .d.wheel = { dx, dy } };
    return _q(act, cmd);
}

esp_err_t actuator_queue_enter(tusb_actuator_t *act, uint16_t x, uint16_t y, uint16_t mask)
{
    act_cmd_t cmd = { .type = ACT_CMD_ENTER, .d.enter = { x, y, mask } };
    return _q(act, cmd);
}

esp_err_t actuator_queue_leave(tusb_actuator_t *act)
{
    act_cmd_t cmd = { .type = ACT_CMD_LEAVE };
    return _q(act, cmd);
}

esp_err_t actuator_queue_jiggle(tusb_actuator_t *act)
{
    act_cmd_t cmd = { .type = ACT_CMD_JIGGLE };
    return _q(act, cmd);
}

esp_err_t actuator_queue_connected(tusb_actuator_t *act)
{
    act_cmd_t cmd = { .type = ACT_CMD_CONNECTED };
    return _q(act, cmd);
}

esp_err_t actuator_queue_disconnected(tusb_actuator_t *act)
{
    /* Flush the entire queue on disconnect - stale events must not survive reconnect */
    act_cmd_t dummy;
    while (xQueueReceive(act->cmd_queue, &dummy, 0) == pdTRUE) {}
    act_cmd_t cmd = { .type = ACT_CMD_DISCONNECTED };
    return _q(act, cmd);
}

esp_err_t actuator_queue_set_screen_size(tusb_actuator_t *act, uint16_t w, uint16_t h)
{
    act_cmd_t cmd = { .type = ACT_CMD_SET_SCREEN_SIZE, .d.screen_size = { w, h } };
    return _q(act, cmd);
}

esp_err_t actuator_queue_set_scaling(tusb_actuator_t *act, uint16_t scaling)
{
    act_cmd_t cmd = { .type = ACT_CMD_SET_SCALING, .d.scale = { scaling } };
    return _q(act, cmd);
}

/* Movement - direct shared state, no queue. Uses atomic tri-state CAS:
 * 0 = sleep/clear, 1 = awake/pending, 2 = sleep/pending (need WAKE)
 * Network task CASes to decide if a WAKE queue event is needed. */

static void _notify_movement(tusb_actuator_t *act)
{
    uint32_t expected = 0;
    if (atomic_compare_exchange_weak_explicit(
            &act->movement_pending, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        xTaskNotifyGive(act->hid_task);
    }
    /* If CAS failed (already 1), movement is already pending - nothing to do. */
}

void actuator_set_movement(tusb_actuator_t *act, uint16_t x, uint16_t y)
{
    act->target_x = x;
    act->target_y = y;
    _notify_movement(act);
}

void actuator_move_relative(tusb_actuator_t *act, int16_t dx, int16_t dy)
{
    int32_t nx = (int32_t)act->target_x + dx;
    int32_t ny = (int32_t)act->target_y + dy;
    act->target_x = (uint16_t)clamp_u16(nx, act->screen_width - 1);
    act->target_y = (uint16_t)clamp_u16(ny, act->screen_height - 1);
    _notify_movement(act);
}

/* ------------------------------------------------------------------ */
/* HID task (Core 1) - processes queue + movement                      */
/* ------------------------------------------------------------------ */

static void hid_task(void *arg)
{
    tusb_actuator_t *act = (tusb_actuator_t *)arg;
    act_cmd_t cmd;
    uint32_t exp;
    uint32_t ul_notif;

    while (true) {
        /* CAS: 1->0 - clear pending movement */
        exp = 1;
        if (atomic_compare_exchange_weak_explicit(
                &act->movement_pending, &exp, 0,
                memory_order_acq_rel, memory_order_relaxed)) {
            if (tud_mounted()) {
                send_abs_mouse_once(act);
            }
        }

        /* Try to drain queue. Only sleep when queue is empty. */
        bool got_cmd = (xQueueReceive(act->cmd_queue, &cmd, 0) == pdTRUE);
        if (!got_cmd) {
            /* Queue empty - sleep until woken by notification or timeout */
            (void)xTaskNotifyWait(0, 0, &ul_notif, pdMS_TO_TICKS(1000));
            /* After waking, check movement + queue again */
            continue;
        }

        if (!tud_mounted()) continue;

        switch (cmd.type) {
            case ACT_CMD_KEY_DOWN: {
                uint16_t key = cmd.d.k.key;
                uint16_t mask = cmd.d.k.mask;
                (void)cmd.d.k.button;

                /* Track by barrier key code */
                if (act->key_count < HID_MAX_KEYS) {
                    bool found = false;
                    for (uint8_t i = 0; i < act->key_count; i++) {
                        if (act->pressed_keys[i] == key) { found = true; break; }
                    }
                    if (!found) {
                        act->pressed_keys[act->key_count++] = key;
                    }
                }

                uint8_t modifier = mask_to_tusb_modifier(mask);
                uint8_t report[KEYBOARD_REPORT_LEN];
                report[0] = modifier;
                report[1] = 0;
                for (uint8_t i = 0; i < 6; i++) {
                    uint16_t k = act->pressed_keys[i];
                    uint8_t hid = k ? barrier_key_to_hid(k) : 0;
                    // Merge modifier keys (0xE0-0xE7) into report[0] instead of key slots
                    if (hid >= 0xE0 && hid <= 0xE7) {
                        report[0] |= (uint8_t)(1u << (hid - 0xE0));
                        hid = 0;
                    }
                    report[2 + i] = hid;
                }
                send_kb_report(act, report);
                break;
            }

            case ACT_CMD_KEY_UP: {
                uint16_t key = cmd.d.k.key;
                uint16_t mask = cmd.d.k.mask;
                (void)cmd.d.k.button;

                bool found = false;
                for (uint8_t i = 0; i < act->key_count; i++) {
                    if (act->pressed_keys[i] == key) {
                        act->pressed_keys[i] = act->pressed_keys[act->key_count - 1];
                        act->pressed_keys[act->key_count - 1] = 0;
                        act->key_count--;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    act->key_count = 0;
                    memset(act->pressed_keys, 0, sizeof(act->pressed_keys));
                }

                uint8_t modifier = mask_to_tusb_modifier(mask);
                uint8_t report[KEYBOARD_REPORT_LEN];
                report[0] = modifier;
                report[1] = 0;
                for (uint8_t i = 0; i < 6; i++) {
                    uint16_t k = act->pressed_keys[i];
                    uint8_t hid = k ? barrier_key_to_hid(k) : 0;
                    if (hid >= 0xE0 && hid <= 0xE7) {
                        report[0] |= (uint8_t)(1u << (hid - 0xE0));
                        hid = 0;
                    }
                    report[2 + i] = hid;
                }
                send_kb_report(act, report);
                break;
            }

            case ACT_CMD_KEY_REPEAT:
                /* Host OS handles native key repeat */
                break;

            case ACT_CMD_MOUSE_DOWN: {
                uint8_t hid_btn = barrier_button_to_hid(cmd.d.btn.id);
                act->mouse_buttons |= hid_btn;
                send_abs_mouse_retry(act);
                break;
            }

            case ACT_CMD_MOUSE_UP: {
                uint8_t hid_btn = barrier_button_to_hid(cmd.d.btn.id);
                act->mouse_buttons &= ~hid_btn;
                send_abs_mouse_retry(act);
                break;
            }

            case ACT_CMD_MOUSE_WHEEL: {
                act->mouse_wheel = clamp_int8(cmd.d.wheel.dy);
                send_abs_mouse_retry(act);
                break;
            }

            case ACT_CMD_ENTER: {
                act->target_x = cmd.d.enter.x;
                act->target_y = cmd.d.enter.y;

                /* Send position directly */
                send_abs_mouse_once(act);

                /* Press modifiers */
                if (cmd.d.enter.mask) {
                    uint8_t modifier = mask_to_tusb_modifier(cmd.d.enter.mask);
                    uint8_t report[KEYBOARD_REPORT_LEN] = { modifier, 0, 0, 0, 0, 0, 0, 0 };
                    send_kb_report(act, report);
                }
                break;
            }

            case ACT_CMD_LEAVE: {
                /* Release all keys */
                act->key_count = 0;
                memset(act->pressed_keys, 0, sizeof(act->pressed_keys));
                uint8_t kbr[KEYBOARD_REPORT_LEN] = { 0 };
                send_kb_report(act, kbr);

                /* Release all mouse buttons */
                act->mouse_buttons = 0;
                act->mouse_wheel = 0;
                send_abs_mouse_once(act);
                break;
            }

            case ACT_CMD_JIGGLE: {
                /* Non-blocking jiggle: move 1px, then back. No delay to avoid
                 * blocking HID task. The server will overwrite position anyway. */
                uint16_t orig_x = act->target_x;
                uint16_t orig_y = act->target_y;
                uint16_t x = (orig_x + JIGGLE_DELTA < act->screen_width)
                             ? orig_x + JIGGLE_DELTA : orig_x - JIGGLE_DELTA;
                act->target_x = x;
                act->target_y = orig_y;
                send_abs_mouse_once(act);
                /* Move back immediately */
                act->target_x = orig_x;
                act->target_y = orig_y;
                send_abs_mouse_once(act);
                break;
            }

            case ACT_CMD_CONNECTED:
                break;

            case ACT_CMD_DISCONNECTED:
                act->key_count = 0;
                memset(act->pressed_keys, 0, sizeof(act->pressed_keys));
                act->mouse_buttons = 0;
                act->mouse_wheel = 0;
                break;

            case ACT_CMD_SET_SCREEN_SIZE:
                act->screen_width  = cmd.d.screen_size.w;
                act->screen_height = cmd.d.screen_size.h;
                break;

            case ACT_CMD_SET_SCALING:
                act->scaling = cmd.d.scale.scaling;
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Init / lifecycle                                                    */
/* ------------------------------------------------------------------ */

esp_err_t tusb_actuator_init(tusb_actuator_t *act, uint16_t screen_width, uint16_t screen_height)
{
    ESP_RETURN_ON_FALSE(act, ESP_ERR_INVALID_ARG, TAG, "act is NULL");

    act->screen_width    = screen_width;
    act->screen_height   = screen_height;
    act->scaling         = 100;
    act->target_x        = 0;
    act->target_y        = 0;
    atomic_store(&act->movement_pending, 0);
    act->mouse_buttons   = 0;
    act->mouse_wheel     = 0;
    act->key_count       = 0;
    act->last_sent_hx    = 0;
    act->last_sent_hy    = 0;
    act->last_sent_buttons = 0;
    memset(act->pressed_keys, 0, sizeof(act->pressed_keys));

    act->cmd_queue = xQueueCreate(ACT_IMPORTANT_QUEUE_DEPTH, sizeof(act_cmd_t));
    ESP_RETURN_ON_FALSE(act->cmd_queue, ESP_ERR_NO_MEM, TAG, "queue create failed");

    act->hid_task = NULL;
    return ESP_OK;
}

esp_err_t actuator_hid_task_start(tusb_actuator_t *act)
{
    ESP_RETURN_ON_FALSE(act && !act->hid_task, ESP_ERR_INVALID_ARG, TAG, "already started");

    BaseType_t xret = xTaskCreatePinnedToCore(
        hid_task,
        "hid_task",
        4096,
        act,
        5,
        &act->hid_task,
        1  /* Core 1 */
    );
    tusb_set_hid_task_handle(act->hid_task);
    return xret == pdPASS ? ESP_OK : ESP_FAIL;
}

void actuator_hid_task_stop(tusb_actuator_t *act)
{
    if (act && act->hid_task) {
        vTaskDelete(act->hid_task);
        act->hid_task = NULL;
    }
    if (act && act->cmd_queue) {
        vQueueDelete(act->cmd_queue);
        act->cmd_queue = NULL;
    }
}
