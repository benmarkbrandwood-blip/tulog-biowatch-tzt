#pragma once

/* -------------------------------------------------------------------------- */
/* AXP2101 PMU battery reader.                                                */
/*                                                                            */
/* Battery state on this board is monitored by the AXP2101 power management   */
/* IC over the BSP shared I2C bus (GPIO14/15) at address 0x34. The previous   */
/* ADC implementation tried GPIO1, which is the SD CMD line — it returned     */
/* nonsense (~6.36 V) and is no longer used.                                  */
/*                                                                            */
/* The function names retain their `battery_adc_*` prefix for compatibility   */
/* with existing callers; despite the name, no ADC is actually used.          */
/* -------------------------------------------------------------------------- */

void  battery_adc_init(void);
float battery_read_voltage(void);
int   battery_read_percent(void);
