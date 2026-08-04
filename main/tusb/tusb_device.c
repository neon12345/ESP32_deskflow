/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "tusb_device.h"

#include "esp_log.h"
#include "esp_err.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include "freertos/task.h"

#include "soc/usb_serial_jtag_struct.h"
#include "soc/lp_system_struct.h"

static const char *TAG = "tusb_device";

#define TUSB_MAX_POWER_MA    100
#define TUSB_HID_EP_ADDR     0x81
#define TUSB_HID_EP_SIZE     16
#define TUSB_HID_POLL_MS     10

static bool g_suspended = false;
static bool g_remote_wakeup_en = false;
static volatile bool g_pending_send = false;

void tusb_set_pending_send(bool pending)
{
    g_pending_send = pending;
}

bool tusb_try_remote_wakeup(void)
{
    if (!g_suspended || !g_remote_wakeup_en || !g_pending_send) return false;
    return tud_remote_wakeup();
}

/* HID task handle - set by actuator, used by tud_hid_report_complete_cb to wake it */
static TaskHandle_t g_hid_task_handle = NULL;

void tusb_set_hid_task_handle(void *handle)
{
    g_hid_task_handle = (TaskHandle_t)handle;
}

/* HID report descriptor - Keyboard + Tablet Pen + Scroll Wheel (all one interface) */
static const uint8_t hid_report_descriptor_main[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(DF_HID_KEYBOARD)),
    // Absolute Mouse - buttons + absolute X/Y + wheel (Report ID 4)
    0x05, 0x01,            // USAGE_PAGE (Generic Desktop)
    0x09, 0x02,            // USAGE (Mouse)
    0xa1, 0x01,            // COLLECTION (Application)
    0x85, 0x04,            //   REPORT_ID (4)
    0x09, 0x01,            //   USAGE (Pointer)
    0xa1, 0x00,            //   COLLECTION (Physical)
    0x05, 0x09,            //     USAGE_PAGE (Button)
    0x19, 0x01,            //     USAGE_MINIMUM (Button 1)
    0x29, 0x03,            //     USAGE_MAXIMUM (Button 3)
    0x15, 0x00,            //     LOGICAL_MINIMUM (0)
    0x25, 0x01,            //     LOGICAL_MAXIMUM (1)
    0x95, 0x03,            //     REPORT_COUNT (3)
    0x75, 0x01,            //     REPORT_SIZE (1)
    0x81, 0x02,            //     INPUT (Data, Var, Abs)
    0x95, 0x01,            //     REPORT_COUNT (1)
    0x75, 0x05,            //     REPORT_SIZE (5)
    0x81, 0x03,            //     INPUT (Const, Var, Abs) - padding
    0x05, 0x01,            //     USAGE_PAGE (Generic Desktop)
    0x09, 0x30,            //     USAGE (X)
    0x09, 0x31,            //     USAGE (Y)
    0x15, 0x00,            //     LOGICAL_MINIMUM (0)
    0x26, 0xff, 0x7f,      //     LOGICAL_MAXIMUM (32767)
    0x75, 0x10,            //     REPORT_SIZE (16)
    0x95, 0x02,            //     REPORT_COUNT (2)
    0x81, 0x02,            //     INPUT (Data, Var, Abs)
    0x15, 0x81,            //     LOGICAL_MINIMUM (-127)
    0x25, 0x7f,            //     LOGICAL_MAXIMUM (127)
    0x09, 0x38,            //     USAGE (Wheel)
    0x75, 0x08,            //     REPORT_SIZE (8)
    0x95, 0x01,            //     REPORT_COUNT (1)
    0x81, 0x06,            //     INPUT (Data, Var, Rel)
    0xc0,                  //   END_COLLECTION (Physical)
    0xc0                   // END_COLLECTION (Application)
};

/* Configuration descriptor - single HID interface */
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, TUSB_MAX_POWER_MA),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor_main),
                       TUSB_HID_EP_ADDR, TUSB_HID_EP_SIZE, TUSB_HID_POLL_MS),
};

/* String descriptors */
const char *hid_string_descriptor[] = {
    (char[]){0x09, 0x04},    // 0: Language (English)
    "Espressif",             // 1: Manufacturer
    "deskflow client",       // 2: Product
    "deskflow-001",          // 3: Serial
    "Barrier HID Interface", // 4: HID
};

/* TinyUSB HID callbacks (must be at file scope) */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void) instance;
    return hid_report_descriptor_main;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void) instance; (void) report_id; (void) report_type;
    (void) buffer; (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void) instance; (void) report_id; (void) report_type;
    (void) buffer; (void) bufsize;
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    g_suspended = true;
    g_remote_wakeup_en = remote_wakeup_en;
    if (g_pending_send && g_remote_wakeup_en) {
        tud_remote_wakeup();
    }
    ESP_LOGI(TAG, "Device suspended, remote_wakeup=%s", remote_wakeup_en ? "yes" : "no");
}

void tud_resume_cb(void)
{
    g_suspended = false;
    ESP_LOGI(TAG, "Device resumed");
}

/* Wake HID task when a report transfer completes so it can send the next one */
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len)
{
    (void)instance; (void)report; (void)len;
    if (g_hid_task_handle) xTaskNotifyGive((TaskHandle_t)g_hid_task_handle);
}

/* Init - follows tusb_hid example exactly */
esp_err_t tusb_device_init(void)
{
    // Swap USB PHY mapping so TinyUSB gets GPIO 24/25 (PHY 0)
    // and USB Serial/JTAG moves to GPIO 26/27 (PHY 1, not connected)
    LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 1;   // Enable SW control of PHY routing
    LP_SYS.usb_ctrl.sw_usb_phy_sel = 1;       // USJ->PHY1(26/27), OTG->PHY0(24/25)
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 0; // Disable USJ pads on GPIO 26/27

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();

    // Use FS port (GPIO 24/25) - the physical USB-C connector on the board
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;

    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
    tusb_cfg.descriptor.string = hid_string_descriptor;
    tusb_cfg.descriptor.string_count =
        sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = hid_configuration_descriptor;
#endif

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    ESP_LOGI(TAG, "TinyUSB device initialized");
    return ESP_OK;
}

bool tusb_device_is_mounted(void)
{
    return tud_mounted();
}

bool tusb_device_is_suspended(void)
{
    return g_suspended;
}

void tusb_device_deinit(void)
{
    tinyusb_driver_uninstall();
    g_suspended = false;
}
