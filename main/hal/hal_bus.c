#include "hal_bus.h"
#include "app_config.h"

#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include <string.h>

static const char *TAG = "hal_bus";

/* ── I2C ─────────────────────────────────────────────────────────────────── */

#define I2C_MAX_DEVS    4
#define I2C_TIMEOUT_MS  50
#define I2C_TX_MAX      64   /* max bytes in a single reg-write burst */

typedef struct {
    uint8_t                 addr;
    i2c_master_dev_handle_t hdl;
} i2c_slot_t;

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_slot_t              s_devs[I2C_MAX_DEVS];
static int                     s_ndevs = 0;

static esp_err_t i2c_add_device(uint8_t addr)
{
    if (s_ndevs >= I2C_MAX_DEVS) return ESP_ERR_NO_MEM;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = I2C_BUS_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &cfg,
                                               &s_devs[s_ndevs].hdl);
    if (err == ESP_OK) {
        s_devs[s_ndevs].addr = addr;
        s_ndevs++;
    }
    return err;
}

static i2c_master_dev_handle_t i2c_find_dev(uint8_t addr)
{
    for (int i = 0; i < s_ndevs; i++) {
        if (s_devs[i].addr == addr) return s_devs[i].hdl;
    }
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t hal_bus_init(void)
{
    /* SPI2 is already initialised by hal_display_init() — nothing to add here.
     * Biosensor SPI devices are registered by their own drivers via
     * hal_spi_dev_add(SPI2_HOST, ...) when those drivers initialise. */

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C0 init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Pre-register known I2C device addresses so hal_i2c_reg_read/write work
     * as soon as the sensor drivers call them. */
    i2c_add_device(I2C_ADDR_MAX30102);
    i2c_add_device(I2C_ADDR_MPU6050);

    /* Probe known addresses — expected to NACK until sensors are physically
     * wired.  Log result; never assert on probe failure. */
    const uint8_t probes[] = { I2C_ADDR_MAX30102, I2C_ADDR_MPU6050, 0x69 };
    for (size_t i = 0; i < sizeof(probes); i++) {
        esp_err_t r = i2c_master_probe(s_i2c_bus, probes[i], 20);
        ESP_LOGI(TAG, "I2C probe 0x%02X: %s",
                 probes[i], (r == ESP_OK) ? "ACK" : "no device");
    }

    ESP_LOGI(TAG, "I2C0 OK — SDA=GPIO%d SCL=GPIO%d @ %d Hz",
             PIN_I2C_SDA, PIN_I2C_SCL, I2C_BUS_FREQ_HZ);
    return ESP_OK;
}

esp_err_t hal_spi_dev_add(spi_host_device_t host,
                            const spi_device_interface_config_t *cfg,
                            spi_device_handle_t *out)
{
    return spi_bus_add_device(host, cfg, out);
}

esp_err_t hal_spi_txrx(spi_device_handle_t dev,
                         const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_transmit(dev, &t);
}

esp_err_t hal_i2c_reg_read(uint8_t addr, uint8_t reg,
                              uint8_t *buf, size_t len)
{
    i2c_master_dev_handle_t hdl = i2c_find_dev(addr);
    if (!hdl) return ESP_ERR_NOT_FOUND;
    return i2c_master_transmit_receive(hdl, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t hal_i2c_reg_write(uint8_t addr, uint8_t reg,
                               const uint8_t *buf, size_t len)
{
    if (len + 1 > I2C_TX_MAX) return ESP_ERR_INVALID_SIZE;
    i2c_master_dev_handle_t hdl = i2c_find_dev(addr);
    if (!hdl) return ESP_ERR_NOT_FOUND;
    uint8_t tx[I2C_TX_MAX];
    tx[0] = reg;
    memcpy(tx + 1, buf, len);
    return i2c_master_transmit(hdl, tx, 1 + len, I2C_TIMEOUT_MS);
}
