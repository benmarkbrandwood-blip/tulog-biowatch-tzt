#include "ui_common.h"

#include <stdio.h>

void style_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, COLOUR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
}

void style_button(lv_obj_t *btn, lv_color_t bg, lv_color_t border)
{
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, border, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
}

void style_card(lv_obj_t *obj, int radius)
{
    lv_obj_set_style_bg_color(obj, COLOUR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
}

void make_title(lv_obj_t *parent, const char *txt, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y);
}

void safe_set_label(lv_obj_t *label, const char *txt)
{
    if (label) {
        lv_label_set_text(label, txt ? txt : "");
    }
}

void format_drift_text(char *out, size_t out_len, int32_t drift_ms)
{
    if (!out || out_len == 0) {
        return;
    }

    int32_t abs_ms = drift_ms >= 0 ? drift_ms : -drift_ms;
    if (abs_ms < 1000) {
        snprintf(out, out_len, "SD %ld ms", (long)drift_ms);
    } else {
        float sec = (float)drift_ms / 1000.0f;
        snprintf(out, out_len, "SD %.2f s", sec);
    }
}
