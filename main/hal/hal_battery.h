#pragma once

/* Battery monitoring for TZT ESP32-2432S024C.
 *
 * This board uses an IP5306 boost+charger IC with no I2C interface visible
 * in the schematic.  GPIO35 connects to the external expansion connector P3,
 * not to a battery voltage sense node.  All functions below return sentinel
 * values (-1 / -1.0f) permanently; callers must handle these gracefully. */

void  battery_adc_init(void);
float battery_read_voltage(void);   /* returns -1.0f — no sense path */
int   battery_read_percent(void);   /* returns -1  — no sense path */
