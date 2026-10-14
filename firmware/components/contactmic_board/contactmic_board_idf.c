#include "contactmic_board_idf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CMQ_I2C_TIMEOUT_MS 20
static cmq_bus_result convert(esp_err_t rc)
{
    if (rc == ESP_OK) return CMQ_BUS_OK;
    if (rc == ESP_ERR_NOT_FOUND || rc == ESP_ERR_INVALID_RESPONSE) return CMQ_BUS_ABSENT;
    return CMQ_BUS_ERROR;
}
static cmq_bus_result probe(void *ctx, uint8_t address)
{
    cmq_idf_bus *b = ctx;
    if (!b || !b->handle || address < 0x6a || address > 0x6b) return CMQ_BUS_ERROR;
    return convert(i2c_master_probe(b->handle, address, CMQ_I2C_TIMEOUT_MS));
}
static cmq_bus_result read_bytes(void *ctx, uint8_t address, uint8_t reg, uint8_t *out, size_t n)
{
    cmq_idf_bus *b = ctx;
    if (!b || address < 0x6a || address > 0x6b || !out || !n || n > 2 ||
        !b->devices[address - 0x6a]) return CMQ_BUS_ERROR;
    return convert(i2c_master_transmit_receive(b->devices[address - 0x6a], &reg, 1,
                                              out, n, CMQ_I2C_TIMEOUT_MS));
}
static cmq_bus_result write_byte(void *ctx, uint8_t address, uint8_t reg, uint8_t value)
{
    cmq_idf_bus *b = ctx;
    if (!b || address < 0x6a || address > 0x6b || !b->devices[address - 0x6a])
        return CMQ_BUS_ERROR;
    const uint8_t bytes[2] = {reg, value};
    return convert(i2c_master_transmit(b->devices[address - 0x6a], bytes, sizeof(bytes),
                                      CMQ_I2C_TIMEOUT_MS));
}
static void delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    /* Round up and add a tick for the current partially elapsed tick. Never busy-spin. */
    uint64_t ticks = ((uint64_t)us * configTICK_RATE_HZ + 999999u) / 1000000u + 1u;
    vTaskDelay((TickType_t)ticks);
}
esp_err_t cmq_idf_close(cmq_idf_bus *b)
{
    if (!b) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < 2; ++i) {
        if (!b->devices[i]) continue;
        esp_err_t rc = i2c_master_bus_rm_device(b->devices[i]);
        if (rc == ESP_OK) b->devices[i] = NULL;
        else result = rc;
    }
    if (!b->devices[0] && !b->devices[1] && b->handle) {
        esp_err_t rc = i2c_del_master_bus(b->handle);
        if (rc == ESP_OK) b->handle = NULL;
        else result = rc;
    }
    return result;
}
esp_err_t cmq_idf_open(cmq_idf_bus *b, uint32_t hz, cmq_bus *out)
{
    if (!b || !out || !hz || hz > CMQ_I2C_MAX_HZ) return ESP_ERR_INVALID_ARG;
    if (b->handle || b->devices[0] || b->devices[1]) return ESP_ERR_INVALID_STATE;
    i2c_master_bus_config_t config = {
        .i2c_port = -1, .sda_io_num = CMQ_SDA_GPIO, .scl_io_num = CMQ_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false
    };
    /* The stock board supplies external pull-ups. No pins, LEDs or interrupts
     * other than this I2C bus are configured by this adapter. */
    esp_err_t rc = i2c_new_master_bus(&config, &b->handle);
    if (rc != ESP_OK) return rc;
    for (uint8_t i = 0; i < 2; ++i) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = (uint16_t)(0x6a + i), .scl_speed_hz = hz
        };
        rc = i2c_master_bus_add_device(b->handle, &dev, &b->devices[i]);
        if (rc != ESP_OK) { (void)cmq_idf_close(b); return rc; }
    }
    *out = (cmq_bus){b, probe, read_bytes, write_byte, delay_us};
    return ESP_OK;
}
