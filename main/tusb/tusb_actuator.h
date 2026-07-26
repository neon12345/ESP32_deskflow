#ifndef TUSB_ACTUATOR_H
#define TUSB_ACTUATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define ACT_IMPORTANT_QUEUE_DEPTH 32

/* Commands that MUST be delivered in order (keys, buttons, wheel) */
typedef enum {
    ACT_CMD_KEY_DOWN,
    ACT_CMD_KEY_UP,
    ACT_CMD_KEY_REPEAT,
    ACT_CMD_MOUSE_DOWN,
    ACT_CMD_MOUSE_UP,
    ACT_CMD_MOUSE_WHEEL,
    ACT_CMD_ENTER,
    ACT_CMD_LEAVE,
    ACT_CMD_JIGGLE,
    ACT_CMD_CONNECTED,
    ACT_CMD_DISCONNECTED,
    ACT_CMD_SET_SCREEN_SIZE,
    ACT_CMD_SET_SCALING,
} act_cmd_type_t;

typedef struct {
    act_cmd_type_t type;
    union {
        struct { uint16_t key; uint16_t mask; uint16_t button; } k;
        struct { int8_t id; } btn;
        struct { int16_t dx; int16_t dy; } wheel;
        struct { uint16_t x; uint16_t y; uint16_t mask; } enter;
        struct { uint16_t w; uint16_t h; } screen_size;
        struct { uint16_t scaling; } scale;
        struct { uint16_t key; uint16_t mask; uint16_t button; uint16_t count; } krep;
    } d;
} act_cmd_t;

/* Actuator state - movement is direct, important events go through queue */
typedef struct {
    uint16_t screen_width;
    uint16_t screen_height;
    uint16_t scaling;

    /* Movement - written by network task, read by HID task. No queue. */
    volatile uint16_t target_x;
    volatile uint16_t target_y;
    _Atomic uint32_t  movement_pending;  /* 0=clear 1=pending movement */

    /* HID task owns these during processing */
    uint8_t  mouse_buttons;
    int8_t   mouse_wheel;
    uint16_t pressed_keys[6];
    uint8_t  key_count;
    uint16_t last_sent_hx;
    uint16_t last_sent_hy;
    uint8_t  last_sent_buttons;

    /* Last queued per type - used to reject duplicates at queue entry */
    uint64_t last_queued_kb_time;   /* ms timestamp - int64_t from esp_timer_get_time() */
    uint16_t last_queued_kb_key;
    uint16_t last_queued_kb_mask;
    uint16_t last_queued_kb_type;  /* ACT_CMD_KEY_DOWN or ACT_CMD_KEY_UP */
    uint64_t last_queued_btn_time;
    int8_t   last_queued_btn_id;
    uint16_t last_queued_btn_type; /* ACT_CMD_MOUSE_DOWN or ACT_CMD_MOUSE_UP */
    uint64_t last_queued_wheel_time;
    int16_t  last_queued_wheel_dy;

    /* Important event queue - HID task processes sequentially with retry */
    QueueHandle_t cmd_queue;
    TaskHandle_t  hid_task;
} tusb_actuator_t;

esp_err_t tusb_actuator_init(tusb_actuator_t *act, uint16_t screen_width, uint16_t screen_height);

/* Start/stop the HID task (runs on Core 1) */
esp_err_t actuator_hid_task_start(tusb_actuator_t *act);
void      actuator_hid_task_stop(tusb_actuator_t *act);

/* --- Network task calls these to queue commands --- */

esp_err_t actuator_queue_key_down(tusb_actuator_t *act, uint16_t key, uint16_t mask, uint16_t button);
esp_err_t actuator_queue_key_up(tusb_actuator_t *act, uint16_t key, uint16_t mask, uint16_t button);
esp_err_t actuator_queue_key_repeat(tusb_actuator_t *act, uint16_t key, uint16_t mask, uint16_t button, uint16_t count);
esp_err_t actuator_queue_mouse_down(tusb_actuator_t *act, int8_t button);
esp_err_t actuator_queue_mouse_up(tusb_actuator_t *act, int8_t button);
esp_err_t actuator_queue_mouse_wheel(tusb_actuator_t *act, int16_t dx, int16_t dy);
esp_err_t actuator_queue_enter(tusb_actuator_t *act, uint16_t x, uint16_t y, uint16_t mask);
esp_err_t actuator_queue_leave(tusb_actuator_t *act);
esp_err_t actuator_queue_jiggle(tusb_actuator_t *act);
esp_err_t actuator_queue_connected(tusb_actuator_t *act);
esp_err_t actuator_queue_disconnected(tusb_actuator_t *act);
esp_err_t actuator_queue_set_screen_size(tusb_actuator_t *act, uint16_t w, uint16_t h);
esp_err_t actuator_queue_set_scaling(tusb_actuator_t *act, uint16_t scaling);

/* --- Movement - direct write, no queue --- */
void actuator_set_movement(tusb_actuator_t *act, uint16_t x, uint16_t y);
void actuator_move_relative(tusb_actuator_t *act, int16_t dx, int16_t dy);

#endif
