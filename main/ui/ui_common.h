#pragma once

#include <stdint.h>
#include <stddef.h>
#include "lvgl.h"

/* -------------------------------------------------------------------------- */
/* Colour palette — high contrast for ILI9341 TFT                           */
/* -------------------------------------------------------------------------- */

#define COLOUR_BG       lv_color_hex(0x000000)   /* true black screen bg */
#define COLOUR_SURFACE  lv_color_hex(0x0E1824)   /* card / panel bg */
#define COLOUR_SURFACE2 lv_color_hex(0x1C2C40)   /* button bg */
#define COLOUR_ACCENT   lv_color_hex(0x4DACFF)   /* bright blue */
#define COLOUR_SUCCESS  lv_color_hex(0x40CC60)   /* bright green */
#define COLOUR_ERROR    lv_color_hex(0xFF4D4D)   /* bright red */
#define COLOUR_TEXT     lv_color_hex(0xFFFFFF)   /* pure white */
#define COLOUR_SUBTEXT  lv_color_hex(0xA0B0C0)   /* light grey-blue */
#define COLOUR_WARN     lv_color_hex(0xFFB030)   /* bright amber */
#define COLOUR_ECG      lv_color_hex(0x00E868)   /* bright ECG green */
#define COLOUR_PPG      lv_color_hex(0xFF5A30)   /* warm orange-red PPG */
#define COLOUR_RESP     lv_color_hex(0x4DACFF)   /* blue (= accent) */
#define COLOUR_MUTEDTAB lv_color_hex(0x1C2C40)   /* = SURFACE2 */

/* -------------------------------------------------------------------------- */
/* Style helpers                                                              */
/* -------------------------------------------------------------------------- */

void style_screen(lv_obj_t *scr);
void style_button(lv_obj_t *btn, lv_color_t bg, lv_color_t border);
void style_card(lv_obj_t *obj, int radius);
void make_title(lv_obj_t *parent, const char *txt, int y);

/* -------------------------------------------------------------------------- */
/* Label / text helpers                                                       */
/* -------------------------------------------------------------------------- */

void safe_set_label(lv_obj_t *label, const char *txt);
void format_drift_text(char *out, size_t out_len, int32_t drift_ms);
