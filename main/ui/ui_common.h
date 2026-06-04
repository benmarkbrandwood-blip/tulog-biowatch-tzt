#pragma once

#include <stdint.h>
#include <stddef.h>
#include "lvgl.h"

/* -------------------------------------------------------------------------- */
/* Colour palette                                                             */
/* -------------------------------------------------------------------------- */

#define COLOUR_BG       lv_color_hex(0x0D1117)
#define COLOUR_SURFACE  lv_color_hex(0x161B22)
#define COLOUR_SURFACE2 lv_color_hex(0x1F2630)
#define COLOUR_ACCENT   lv_color_hex(0x58A6FF)
#define COLOUR_SUCCESS  lv_color_hex(0x3FB950)
#define COLOUR_ERROR    lv_color_hex(0xF85149)
#define COLOUR_TEXT     lv_color_hex(0xE6EDF3)
#define COLOUR_SUBTEXT  lv_color_hex(0x8B949E)
#define COLOUR_WARN     lv_color_hex(0xD29922)
#define COLOUR_ECG      lv_color_hex(0x3FB950)
#define COLOUR_PPG      lv_color_hex(0xF85149)
#define COLOUR_RESP     lv_color_hex(0x58A6FF)
#define COLOUR_MUTEDTAB lv_color_hex(0x30363D)

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
