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
    ESP_LOGI(TAG, "Init: probing 0x%02X on I2C0 (SDA=GPIO%d SCL=GPIO%d)",
             I2C_ADDR_MPU6050, PIN_I2C_SDA, PIN_I2C_SCL);

    /* Verify device identity */
    uint8_t who_am_i = 0;
    esp_err_t err = hal_i2c_reg_read(I2C_ADDR_MPU6050, MPU6050_REG_WHO_AM_I,
                                      &who_am_i, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s — check wiring and pull-ups",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WHO_AM_I = 0x%02X (expected 0x%02X) %s",
             who_am_i, MPU6050_WHO_AM_I_VAL,
             (who_am_i == MPU6050_WHO_AM_I_VAL) ? "OK" : "MISMATCH");
    if (who_am_i != MPU6050_WHO_AM_I_VAL) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Wake from sleep: clear SLEEP bit in PWR_MGMT_1 */
    uint8_t pwr = 0x00;
    err = hal_i2c_reg_write(I2C_ADDR_MPU6050, MPU6050_REG_PWR_MGMT_1, &pwr, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWR_MGMT_1 write failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Read back to confirm wake */
    uint8_t pwr_rb = 0xFF;
    hal_i2c_reg_read(I2C_ADDR_MPU6050, MPU6050_REG_PWR_MGMT_1, &pwr_rb, 1);
    ESP_LOGI(TAG, "PWR_MGMT_1 wrote 0x00, readback 0x%02X (SLEEP bit %s)",
             pwr_rb, (pwr_rb & 0x40) ? "SET — still asleep!" : "clear — awake");

    /* Disable gyroscope to save power */
    uint8_t pwr2 = 0x07;
    err = hal_i2c_reg_write(I2C_ADDR_MPU6050, 0x6C, &pwr2, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWR_MGMT_2 write failed: %s (gyro standby skipped)",
                 esp_err_to_name(err));
    }

    /* Set accel full-scale range to ±2g (AFS_SEL bits 4:3 = 00) */
    uint8_t accel_cfg = 0x00;
    err = hal_i2c_reg_write(I2C_ADDR_MPU6050, MPU6050_REG_ACCEL_CONFIG, &accel_cfg, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ACCEL_CONFIG write failed: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t accel_cfg_rb = 0xFF;
    hal_i2c_reg_read(I2C_ADDR_MPU6050, MPU6050_REG_ACCEL_CONFIG, &accel_cfg_rb, 1);
    ESP_LOGI(TAG, "ACCEL_CONFIG wrote 0x00, readback 0x%02X (AFS_SEL=%d, expect 0 for ±2g)",
             accel_cfg_rb, (accel_cfg_rb >> 3) & 0x03);

    s_init_ok = true;
    ESP_LOGI(TAG, "MPU-6050 ready — accel ±2g (16384 LSB/g), gyro standby");
    return ESP_OK;
}

esp_err_t hal_mpu6050_read_accel(int16_t *x, int16_t *y, int16_t *z)
{
    if (!s_init_ok) return ESP_ERR_NOT_FOUND;

    /* Burst-read 6 bytes: XOUT_H, XOUT_L, YOUT_H, YOUT_L, ZOUT_H, ZOUT_L */
    uint8_t buf[6];
    esp_err_t err = hal_i2c_reg_read(I2C_ADDR_MPU6050, MPU6050_REG_ACCEL_XOUT_H,
                                      buf, sizeof(buf));
    if (err != ESP_OK) {
        static uint32_t s_err_ctr = 0;
        s_err_ctr++;
        if (s_err_ctr == 1 || s_err_ctr % 100 == 0) {
            ESP_LOGE(TAG, "read_accel I2C error #%lu: %s",
                     (unsigned long)s_err_ctr, esp_err_to_name(err));
        }
        return err;
    }

    *x = (int16_t)((buf[0] << 8) | buf[1]);
    *y = (int16_t)((buf[2] << 8) | buf[3]);
    *z = (int16_t)((buf[4] << 8) | buf[5]);

    /* Log every ~1 s (100 Hz polling) — raw counts and calibrated m/s² */
    static uint32_t s_read_ctr = 0;
    if (++s_read_ctr % 100 == 1) {
        float ax = *x / HAL_MPU6050_ACCEL_SCALE * HAL_MPU6050_G_MS2;
        float ay = *y / HAL_MPU6050_ACCEL_SCALE * HAL_MPU6050_G_MS2;
        float az = *z / HAL_MPU6050_ACCEL_SCALE * HAL_MPU6050_G_MS2;
        ESP_LOGI(TAG, "accel raw X=%d Y=%d Z=%d  →  %.3f  %.3f  %.3f m/s²",
                 *x, *y, *z, ax, ay, az);
    }

    return ESP_OK;
}
