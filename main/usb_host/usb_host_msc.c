/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"
#include "ffconf.h"
#include "usb_host_msc.h"
#include "config/config.h"

static const char *TAG = "usb_host_msc";

#define MOUNT_PATH_PREFIX   "/usb"
#define MAX_MSC_DEVICES     CONFIG_FATFS_VOLUME_COUNT
#define USB_TASK_STACK      4096
#define USB_TASK_PRIO       5
#define MAX_OPEN_FILES      4
#define ALLOCATION_UNIT     8192
#define ACTIVATE_TIMEOUT_MS 10000

/* ------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------ */
typedef enum {
    USB_MSC_STATE_INACTIVE,
    USB_MSC_STATE_ACTIVATING,
    USB_MSC_STATE_MOUNTED,
    USB_MSC_STATE_ERROR,
} usb_msc_state_t;

typedef struct {
    uint8_t                          usb_addr;
    msc_host_device_handle_t         msc_device;
    msc_host_vfs_handle_t            vfs_handle;
} msc_entry_t;

static msc_entry_t        *s_devices[MAX_MSC_DEVICES] = {0};
static usb_msc_state_t     s_state = USB_MSC_STATE_INACTIVE;
static TaskHandle_t        s_task_handle = NULL;
static QueueHandle_t       s_event_queue = NULL;
static SemaphoreHandle_t   s_mounted_sem = NULL;
static volatile bool       s_stop = false;
static portMUX_TYPE        s_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint8_t                  usb_addr;
    msc_host_device_handle_t handle;
    bool                     connected;
} msc_event_msg_t;

/* ------------------------------------------------------------------
 * Slot helpers
 * ------------------------------------------------------------------ */
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (s_devices[i] == NULL) return i;
    }
    return -1;
}

static int find_slot_by_handle(msc_host_device_handle_t handle)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (s_devices[i] && s_devices[i]->msc_device == handle) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------
 * Connect / Disconnect
 * ------------------------------------------------------------------ */
static esp_err_t handle_device_connected(uint8_t addr)
{
    int slot = find_free_slot();
    if (slot < 0) {
        ESP_LOGW(TAG, "No free slot (max %d)", MAX_MSC_DEVICES);
        return ESP_ERR_NOT_FOUND;
    }

    s_devices[slot] = calloc(1, sizeof(msc_entry_t));
    if (s_devices[slot] == NULL) return ESP_ERR_NO_MEM;

    esp_err_t ret = msc_host_install_device(addr, &s_devices[slot]->msc_device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "install_device failed: %s", esp_err_to_name(ret));
        free(s_devices[slot]);
        s_devices[slot] = NULL;
        return ret;
    }
    s_devices[slot]->usb_addr = addr;

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = MAX_OPEN_FILES,
        .allocation_unit_size = ALLOCATION_UNIT,
    };

    char path[16];
    snprintf(path, sizeof(path), MOUNT_PATH_PREFIX "%d", slot);

    ret = msc_host_vfs_register(s_devices[slot]->msc_device, path,
                                &mount_cfg, &s_devices[slot]->vfs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "vfs_register failed: %s", esp_err_to_name(ret));
        msc_host_uninstall_device(s_devices[slot]->msc_device);
        free(s_devices[slot]);
        s_devices[slot] = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "MSC connected, mounted at %s", path);

    /* Signal that we're ready */
    {
        usb_msc_state_t st;
        taskENTER_CRITICAL(&s_lock);
        st = s_state;
        if (st == USB_MSC_STATE_ACTIVATING) {
            s_state = USB_MSC_STATE_MOUNTED;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (st == USB_MSC_STATE_ACTIVATING) {
            xSemaphoreGive(s_mounted_sem);
        }
    }

    /* Reload config now that filesystem is available */
    config_load();

    return ESP_OK;
}

static void handle_device_disconnected(msc_host_device_handle_t handle)
{
    int slot = find_slot_by_handle(handle);
    if (slot < 0) {
        ESP_LOGW(TAG, "Disconnected device not found");
        return;
    }

    if (s_devices[slot]->vfs_handle) {
        msc_host_vfs_unregister(s_devices[slot]->vfs_handle);
    }
    if (s_devices[slot]->msc_device) {
        msc_host_uninstall_device(s_devices[slot]->msc_device);
    }

    free(s_devices[slot]);
    s_devices[slot] = NULL;
    ESP_LOGI(TAG, "MSC disconnected");
}

/* ------------------------------------------------------------------
 * MSC callback
 * ------------------------------------------------------------------ */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    msc_event_msg_t msg;

    switch (event->event) {
        case MSC_DEVICE_CONNECTED:
            msg.connected = true;
            msg.usb_addr  = event->device.address;
            msg.handle    = event->device.handle;
            break;
        case MSC_DEVICE_DISCONNECTED:
            msg.connected = false;
            msg.handle    = event->device.handle;
            break;
        default:
            return;
    }

    xQueueSend(s_event_queue, &msg, portMAX_DELAY);
}

/* ------------------------------------------------------------------
 * USB host task
 * ------------------------------------------------------------------ */
static void usb_host_task(void *arg)
{
    (void)arg;

    s_stop = false;

    usb_host_config_t host_cfg = { .intr_flags = ESP_INTR_FLAG_LOWMED };
    esp_err_t ret = usb_host_install(&host_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {  // Already installed is OK
        ESP_LOGE(TAG, "usb_host_install failed: %d", ret);
        s_task_handle = NULL;
        s_state = USB_MSC_STATE_ERROR;
        xSemaphoreGive(s_mounted_sem);
        vTaskDelete(NULL);
        return;
    }

    msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true,
        .task_priority         = USB_TASK_PRIO,
        .stack_size            = USB_TASK_STACK,
        .callback              = msc_event_cb,
    };
    ret = msc_host_install(&msc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install failed: %d", ret);
        s_task_handle = NULL;
        s_state = USB_MSC_STATE_ERROR;
        xSemaphoreGive(s_mounted_sem);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB host MSC active (pins 50/49, HS PHY)");

    for (;;) {
        msc_event_msg_t msg;
        uint32_t        flags;

        /* Block up to 100ms so we can poll s_stop */
        BaseType_t got = xQueueReceive(s_event_queue, &msg, pdMS_TO_TICKS(100));
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);

        if (got == pdTRUE) {
            if (msg.connected) {
                handle_device_connected(msg.usb_addr);
            } else {
                handle_device_disconnected(msg.handle);
            }
        }

        if (s_stop) break;
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) &&
            (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)) {
            break;
        }
    }

    /* Cleanup devices */
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (s_devices[i]) {
            handle_device_disconnected(s_devices[i]->msc_device);
        }
    }

    ESP_LOGI(TAG, "Stopping USB host MSC");
    vTaskDelay(pdMS_TO_TICKS(10));
    msc_host_uninstall();

    {
        taskENTER_CRITICAL(&s_lock);
        s_task_handle = NULL;
        taskEXIT_CRITICAL(&s_lock);
    }
    /* Always signal the starter, even on error/timeout, so usb_host_msc_start unblocks */
    xSemaphoreGive(s_mounted_sem);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */
esp_err_t usb_host_msc_init(void)
{
    s_event_queue = xQueueCreate(8, sizeof(msc_event_msg_t));
    if (s_event_queue == NULL) return ESP_ERR_NO_MEM;

    s_mounted_sem = xSemaphoreCreateBinary();
    if (s_mounted_sem == NULL) {
        vQueueDelete(s_event_queue);
        return ESP_ERR_NO_MEM;
    }

    s_state = USB_MSC_STATE_INACTIVE;
    return ESP_OK;
}

esp_err_t usb_host_msc_start(void)
{
    usb_msc_state_t st;
    taskENTER_CRITICAL(&s_lock);
    st = s_state;
    taskEXIT_CRITICAL(&s_lock);

    if (st == USB_MSC_STATE_MOUNTED) return ESP_OK;
    if (st == USB_MSC_STATE_ACTIVATING) return ESP_ERR_INVALID_STATE;

    /* Allow recovery from ERROR state */
    if (st == USB_MSC_STATE_ERROR) {
        taskENTER_CRITICAL(&s_lock);
        s_state = USB_MSC_STATE_INACTIVE;
        taskEXIT_CRITICAL(&s_lock);
    }

    {
        taskENTER_CRITICAL(&s_lock);
        s_state = USB_MSC_STATE_ACTIVATING;
        taskEXIT_CRITICAL(&s_lock);
    }
    s_stop = false;
    xSemaphoreTake(s_mounted_sem, 0); /* ensure empty */

    TaskHandle_t new_handle = NULL;
    BaseType_t ok = xTaskCreate(usb_host_task, "usb_host",
                                USB_TASK_STACK, NULL, USB_TASK_PRIO,
                                &new_handle);
    if (ok != pdPASS) {
        taskENTER_CRITICAL(&s_lock);
        s_state = USB_MSC_STATE_ERROR;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_FAIL;
    }

    {
        taskENTER_CRITICAL(&s_lock);
        s_task_handle = new_handle;
        taskEXIT_CRITICAL(&s_lock);
    }

    /* Wait for mount or error */
    TickType_t timeout = pdMS_TO_TICKS(ACTIVATE_TIMEOUT_MS);
    BaseType_t mounted = xSemaphoreTake(s_mounted_sem, timeout);

    if (mounted != pdTRUE) {
        ESP_LOGW(TAG, "Activate timeout or error");
        s_stop = true;
        /* Drain any stale semaphore from exiting task */
        xSemaphoreTake(s_mounted_sem, portMAX_DELAY);
        /* Give the task time to exit */
        vTaskDelay(pdMS_TO_TICKS(500));
        taskENTER_CRITICAL(&s_lock);
        s_state = USB_MSC_STATE_INACTIVE;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

void usb_host_msc_stop(void)
{
    usb_msc_state_t st;
    taskENTER_CRITICAL(&s_lock);
    st = s_state;
    taskEXIT_CRITICAL(&s_lock);

    if (st == USB_MSC_STATE_INACTIVE) return;

    ESP_LOGI(TAG, "Deactivating USB host MSC");
    s_stop = true;

    /* Wait for task to exit */
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        TaskHandle_t handle;
        taskENTER_CRITICAL(&s_lock);
        handle = s_task_handle;
        taskEXIT_CRITICAL(&s_lock);
        if (handle == NULL) break;
        vTaskDelay(pdMS_TO_TICKS(50));
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(2000)) {
            ESP_LOGW(TAG, "Deactivate timeout");
            break;
        }
    }

    taskENTER_CRITICAL(&s_lock);
    s_state = USB_MSC_STATE_INACTIVE;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "USB host MSC deactivated");
}

bool usb_host_msc_is_mounted(void)
{
    usb_msc_state_t st;
    taskENTER_CRITICAL(&s_lock);
    st = s_state;
    taskEXIT_CRITICAL(&s_lock);
    return st == USB_MSC_STATE_MOUNTED;
}
