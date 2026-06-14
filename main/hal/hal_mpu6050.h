#pragma once
#include "esp_err.h"
#include <stdint.h>

/* Conversion: accel_ms2 = raw / HAL_MPU6050_ACCEL_SCALE * HAL_MPU6050_G_MS2 */
#define HAL_MPU6050_ACCEL_SCALE  16384.0f  /* LSB/g at ±2g full-scale */
#define HAL_MPU6050_G_MS2        9.81f     /* m/s² per g */

/**
 * hal_mpu6050_init — wake MPU-6050 from sleep, configure accel-only ±2g.
 * Call once after hal_bus_init().
 * Returns ESP_ERR_NOT_FOUND if the device does not ACK on I2C.
 */
esp_err_t hal_mpu6050_init(void);

/**
 * hal_mpu6050_read_accel — burst-read all three accelerometer axes.
 *
 * Returns raw int16_t values.  ±32767 = ±2g = ±19.62 m/s².
 * To convert to m/s²: val = raw / HAL_MPU6050_ACCEL_SCALE * HAL_MPU6050_G_MS2
 *
 * Returns ESP_ERR_NOT_FOUND if the device was not successfully initialised.
 */
esp_err_t hal_mpu6050_read_accel(int16_t *x, int16_t *y, int16_t *z);
