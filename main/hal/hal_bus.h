#pragma once
#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"

/**
 * hal_bus_init — initialise I2C0 master bus and log the I2C device scan.
 *
 * SPI2 is already initialised by hal_display_init(); this function does NOT
 * call spi_bus_initialize() again.  Call this once after hal_display_init().
 */
esp_err_t hal_bus_init(void);

/**
 * hal_spi_dev_add — add a sensor device to an existing SPI bus.
 *
 * Thin wrapper around spi_bus_add_device().  Sensor drivers call this in their
 * own init; nothing calls it during Stage 0.
 */
esp_err_t hal_spi_dev_add(spi_host_device_t host,
                           const spi_device_interface_config_t *cfg,
                           spi_device_handle_t *out);

/**
 * hal_spi_txrx — full-duplex SPI transaction.
 *
 * Either tx or rx may be NULL for simplex transfers.  len is in bytes.
 * Never call from an ISR — spi_master uses a FreeRTOS mutex.
 */
esp_err_t hal_spi_txrx(spi_device_handle_t dev,
                        const uint8_t *tx, uint8_t *rx, size_t len);

/**
 * hal_i2c_reg_read — read len bytes from a device register over I2C0.
 * hal_i2c_reg_write — write len bytes to a device register over I2C0.
 *
 * addr is the 7-bit I2C address.  Only addresses registered in hal_bus_init()
 * (I2C_ADDR_MAX30102, I2C_ADDR_MPU6050) are supported.
 */
esp_err_t hal_i2c_reg_read(uint8_t addr, uint8_t reg,
                             uint8_t *buf, size_t len);
esp_err_t hal_i2c_reg_write(uint8_t addr, uint8_t reg,
                              const uint8_t *buf, size_t len);
