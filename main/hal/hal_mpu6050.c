#include "hal_mpu6050.h"
#include "hal_bus.h"
#include "app_config.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "hal_mpu6050";

/* MPU-6050 register addresses */
#define MPU6050_REG_PWR_MGMT_1   0x6B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_GYRO_CONFIG  0x1B
#define MPU6050_REG_ACCEL_XOUT_H 0x3B
#define MPU6050_REG_WHO_AM_I     0x75

/* WHO_AM_I expected response for MPU-6050 (AD0=low → 0x68) */
#define MPU6050_WHO_AM_I_VAL     0x68

static bool s_init_ok = false;

esp_err_t hal_mpu6050_init(void)
{
    /* Verify device identity */
    uint8_t who_am_i = 0;
    esp_err_t err = hal_i2c_reg_read(I2C_ADDR_MPU6050, MPU6050_REG_WHO_AM_I,
                                      &who_am_i, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WHO_AM_I read failed: %s (device not connected?)",
                 esp_err_to_name(err));
        return err;
    }
    if (who_am_i != MPU6050_WHO_AM_I_VAL) {
        ESP_LOGW(TAG, "WHO_AM_I=0x%02X, expected 0x%02X", who_am_i, MPU6050_WHO_AM_I_VAL);
        return ESP_ERR_NOT_FOUND;
    }

    /* Wake from sleep: clear SLEEP bit in PWR_MGMT_1; use internal 8 MHz oscillator */
    uint8_t pwr = 0x00;
    err = hal_i2c_reg_write(I2C_ADDR_MPU6050, MPU6050_REG_PWR_MGMT_1, &pwr, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWR_MGMT_1 write failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Disable gyroscope to save power: set STBY_XG | STBY_YG | STBY_ZG bits
     * in PWR_MGMT_2 (register 0x6C) */
    uint8_t pwr2 = 0x07;  /* bits 2:0 = STBY_XG | STBY_YG | STBY_ZG */
    err = hal_i2c_reg_write(I2C_ADDR_MPU6050, 0x6C, &pwr2, 1);
    if (err != ESP_OK) {
        /* Non-fatal: gyro standby is a power optimisation, not required for accel */
        ESP_LOGW(TAG, "PWR_MGMT_2 write failed: %s", esp_err_to_name(err));
    }

    /* Set accel full-scale range to ±2g (AFS_SEL = 00, bits 4:3 = 0) */
    uint8_t accel_cfg = 0x00;
    err = hal_i2c_reg_write(I2C_ADDR_MPU6050, MPU6050_REG_ACCEL_CONFIG, &accel_cfg, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ACCEL_CONFIG write failed: %s", esp_err_to_name(err));
        return err;
    }

    s_init_ok = true;
    ESP_LOGI(TAG, "MPU-6050 online — accel ±2g, gyro standby");
    return ESP_OK;
}

esp_err_t hal_mpu6050_read_accel(int16_t *x, int16_t *y, int16_t *z)
{
    if (!s_init_ok) return ESP_ERR_NOT_FOUND;

    /* Burst-read 6 bytes: XOUT_H, XOUT_L, YOUT_H, YOUT_L, ZOUT_H, ZOUT_L */
    uint8_t buf[6];
    esp_err_t err = hal_i2c_reg_read(I2C_ADDR_MPU6050, MPU6050_REG_ACCEL_XOUT_H,
                                      buf, sizeof(buf));
    if (err != ESP_OK) return err;

    *x = (int16_t)((buf[0] << 8) | buf[1]);
    *y = (int16_t)((buf[2] << 8) | buf[3]);
    *z = (int16_t)((buf[4] << 8) | buf[5]);
    return ESP_OK;
}
