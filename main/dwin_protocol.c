#include "dwin_protocol.h"

#include "screen_config.h"

#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "dwin";

#if CONFIG_ENABLE_DWIN_DISPLAY

static uart_port_t s_uart_num = UART_NUM_MAX;
static bool s_uart_installed = false;

static void send_frame(const uint8_t *data, size_t len)
{
    if (!s_uart_installed || data == NULL || len == 0) {
        return;
    }
    uart_write_bytes(s_uart_num, data, len);
    uart_wait_tx_done(s_uart_num, pdMS_TO_TICKS(100));
}

esp_err_t dwin_init(void)
{
    if (s_uart_installed) {
        return ESP_OK;
    }

    s_uart_num = (uart_port_t)CONFIG_DWIN_UART_NUM;

    uart_config_t uart_config = {
        .baud_rate = CONFIG_DWIN_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(s_uart_num, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(s_uart_num, &uart_config);
    if (err != ESP_OK) {
        uart_driver_delete(s_uart_num);
        return err;
    }

    err = uart_set_pin(s_uart_num, CONFIG_DWIN_UART_TX_PIN, CONFIG_DWIN_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        uart_driver_delete(s_uart_num);
        return err;
    }

    s_uart_installed = true;
    ESP_LOGI(TAG, "init UART%d baud=%d TX=%d RX=%d",
             (int)s_uart_num, CONFIG_DWIN_UART_BAUD,
             CONFIG_DWIN_UART_TX_PIN, CONFIG_DWIN_UART_RX_PIN);
    return ESP_OK;
}

void dwin_deinit(void)
{
    if (!s_uart_installed) {
        return;
    }
    uart_driver_delete(s_uart_num);
    s_uart_installed = false;
    s_uart_num = UART_NUM_MAX;
}

void dwin_set_page(uint16_t page_id)
{
    uint8_t frame[10] = {
        0x5A, 0xA5,
        0x07,
        0x82,
        0x00, 0x84,
        0x5A, 0x01,
        (uint8_t)(page_id >> 8),
        (uint8_t)(page_id & 0xFF),
    };
    send_frame(frame, sizeof(frame));
}

void dwin_write_word(uint16_t addr, uint16_t value)
{
    uint8_t frame[8] = {
        0x5A, 0xA5,
        0x05,
        0x82,
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFF),
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF),
    };
    send_frame(frame, sizeof(frame));
}

void dwin_send_qr(uint16_t vp, const char *txt, size_t max_url_bytes)
{
    if (txt == NULL) {
        return;
    }

    size_t len = strlen(txt);
    if (len > max_url_bytes) {
        len = max_url_bytes;
    }
    if (len > SCREEN_DGUS_QR_UART_MAX_PER_FRAME) {
        len = SCREEN_DGUS_QR_UART_MAX_PER_FRAME;
    }

    uint8_t frame[8U + SCREEN_DGUS_QR_UART_MAX_PER_FRAME];
    const uint8_t data_len = (uint8_t)(1U + 2U + len + 2U);
    frame[0] = 0x5A;
    frame[1] = 0xA5;
    frame[2] = data_len;
    frame[3] = 0x82;
    frame[4] = (uint8_t)(vp >> 8);
    frame[5] = (uint8_t)(vp & 0xFF);
    memcpy(&frame[6], txt, len);
    frame[6 + len] = 0xFF;
    frame[7 + len] = 0xFF;
    send_frame(frame, 8U + len);
}

#else /* !CONFIG_ENABLE_DWIN_DISPLAY */

esp_err_t dwin_init(void)
{
    return ESP_OK;
}

void dwin_deinit(void) {}
void dwin_set_page(uint16_t page_id) { (void)page_id; }
void dwin_write_word(uint16_t addr, uint16_t value) { (void)addr; (void)value; }
void dwin_send_qr(uint16_t vp, const char *txt, size_t max_url_bytes)
{
    (void)vp;
    (void)txt;
    (void)max_url_bytes;
}

#endif /* CONFIG_ENABLE_DWIN_DISPLAY */
