/**
 * lvgl_ui.cpp - LVGL 9.x UI: Idle / Identifying / Monitoring / Rest pages.
 * 渐变科技风 dark theme. Colors are WYSIWYG since ScreenDriver invert fix (2026-08-04).
 * NOTE: only reuse CJK chars present in the custom fonts (see tools/gen_lvgl_font.py);
 * symbols (LV_SYMBOL_*) live in the Montserrat fonts, Chinese in lv_font_chinese_*.
 */
#include "lvgl_ui.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "ScreenDriver.h"
#include "display_data.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "LVGL_UI";
extern "C" { extern const lv_font_t lv_font_chinese_16; }
extern "C" { extern const lv_font_t lv_font_chinese_16b; }  /* bold (SimHei) */
extern "C" { extern const lv_font_t lv_font_chinese_18b; }  /* bold 18px */
extern "C" { extern const lv_font_t lv_font_fancy_44; }

/* ── R/B channel swap compensation ──
 * This module's ST7789 renders every configured color with R and B swapped
 * (confirmed on hardware 2026-08-04: blue header→orange, yellow→blue, ...).
 * render_as() takes the WYSIWYG color we WANT and returns the value the panel
 * actually needs, so every C_* below is written in plain visual terms.
 * Driver untouched (per user: no more driver fiddling). */
static lv_color_t render_as(uint32_t hex) {
    uint8_t r = (hex >> 16) & 0xFF;
    uint8_t g = (hex >> 8) & 0xFF;
    uint8_t b = hex & 0xFF;
    return lv_color_make(b, g, r);   /* swap R/B */
}
#define C(hex) render_as(hex)

/* ── Theme palette (WYSIWYG colors — R/B swap auto-compensated by C()) ── */
#define C_BG       C(0x0B1220)   /* deep navy background */
#define C_PANEL    C(0x16233C)   /* card */
#define C_BORDER   C(0x2A3D5E)   /* card border / chart grid */
#define C_ACCENT   C(0x3070FF)   /* accent: renders BLUE (PERCLOS curve, symbols) */
#define C_HDR0     C(0x0086E6)   /* header gradient start (blue, white text OK) */
#define C_HDR1     C(0x00308C)   /* header gradient end (navy) */
#define C_TXT      C(0xE8F1FF)   /* main text */
#define C_DIM      C(0x8FA3C0)   /* secondary text */
#define C_GOOD     C(0x35E08A)   /* normal level — renders green */
#define C_WARN     C(0xFFD700)   /* mild level / threshold — renders YELLOW */
#define C_BAD      C(0xFF3030)   /* severe level / threshold — renders RED */

/* ── Screens ── */
static lv_obj_t *scr_idle = nullptr;
static lv_obj_t *scr_identifying = nullptr;
static lv_obj_t *scr_monitoring = nullptr;
static lv_obj_t *scr_rest = nullptr;
static lv_obj_t *scr_active = nullptr;

/* ── Rest widgets ── */
static lv_obj_t *rest_time_elapsed = nullptr;  /* mm:ss elapsed (44px) */
static lv_obj_t *rest_time_total = nullptr;    /* / total */
static lv_obj_t *rest_drive = nullptr;         /* drive minutes snapshot */
static lv_obj_t *rest_loc = nullptr;           /* parking location */
static lv_obj_t *rest_status = nullptr;        /* 强制休息 / 建议休息 / 休息完成 */
static lv_obj_t *rest_bar = nullptr;           /* progress bar */
static lv_obj_t *rest_recovery = nullptr;      /* recovery % */
static lv_obj_t *rest_pcl_before = nullptr;    /* PERCLOS before rest */
static lv_obj_t *rest_hint = nullptr;          /* bottom hint */

/* ── Monitoring widgets ── */
static lv_obj_t *mon_arc = nullptr;            /* PERCLOS arc gauge */
static lv_obj_t *mon_pcl_v = nullptr;          /* gauge center % */
static lv_obj_t *mon_level = nullptr;          /* fatigue level text (colored) */
static lv_obj_t *mon_chart = nullptr;
static lv_chart_series_t *mon_perclos_ser = nullptr;
static lv_obj_t *mon_pred_chart = nullptr;           /* overlay chart: dashed prediction line only */
static lv_chart_series_t *mon_pred_ser = nullptr;    /* predicted PERCLOS series (on overlay chart) */
static lv_chart_series_t *mon_thresh30 = nullptr;
static lv_chart_series_t *mon_thresh60 = nullptr;
static lv_obj_t *mon_spd_v = nullptr;          /* SPEED value */
static lv_obj_t *mon_spd_gps = nullptr;        /* GPS value */
static lv_obj_t *mon_time_v = nullptr;         /* DRIVE value */
static lv_obj_t *mon_drv_num = nullptr;        /* DRIVER number (ASCII, colored by level) */
static lv_obj_t *mon_yawn_cnt = nullptr;        /* yawn count */
static lv_obj_t *mon_yawn_boost = nullptr;      /* yawn PERCLOS boost */
static lv_obj_t *mon_pred = nullptr;             /* fatigue prediction */

/* ── Idle widgets ── */
static lv_obj_t *idle_time = nullptr;     /* big HH:MM:SS (montserrat_48, glowing, blinking colon) */
static lv_obj_t *idle_date = nullptr;
static lv_obj_t *idle_week = nullptr;     /* 周X */
static lv_obj_t *idle_status = nullptr;
static lv_obj_t *idle_coord = nullptr;
static int s_last_sec = -1;               /* clock redraw guard: once per second */

static bool s_last_id = false, s_last_cr = false, s_last_moving = false, s_last_rest = false;
static const int HIST = 120;

/* ── Level helpers ── */
static lv_color_t level_color(uint8_t lvl) {
    if (lvl == 2) return C_BAD;
    if (lvl == 1) return C_WARN;
    return C_GOOD;
}
static const char *level_text(uint8_t lvl) {
    if (lvl == 2) return "严重";
    if (lvl == 1) return "轻度";
    return "正常";
}

/* GPS 状态: no=没收到数据 / mock=模拟 / real=真实卫星定位 */
static const char *gps_status_str(void) {
    if (!g_display_data.gps_connected) return "no";
    return (g_display_data.gps_mode == 0) ? "real" : "mock";
}

/* ── Helpers ── */
static void apply_screen_bg(lv_obj_t *s) {
    lv_obj_set_style_bg_color(s, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
}

/* Gradient header bar (cyan→blue), returns the bar so caller can add status chips */
static lv_obj_t *make_header(lv_obj_t *parent, const char *title, const lv_font_t *font) {
    lv_obj_t *h = lv_obj_create(parent);
    lv_obj_set_size(h, 240, 38);
    lv_obj_set_pos(h, 0, 0);
    lv_obj_set_style_bg_color(h, C_HDR0, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(h, C_HDR1, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(h, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(h, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(h, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(h, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(h, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *t = lv_label_create(h);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 12, 0);
    return h;
}

/* Rounded dark card container (children positioned absolutely) */
static lv_obj_t *make_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, C_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    return c;
}

/* Rounded status chip: dot + text, border in `color` */
static lv_obj_t *make_chip(lv_obj_t *parent, const char *text, lv_color_t color, const lv_font_t *font) {
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_set_size(chip, 78, 22);
    lv_obj_set_style_bg_color(chip, C(0x14304A), LV_PART_MAIN);  /* dark teal chip bg */
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(chip, 11, LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, color, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(chip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *dot = lv_obj_create(chip);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *t = lv_label_create(chip);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_font(t, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, C_TXT, LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_CENTER, 8, 0);
    return chip;
}

/* ── Idle page ── */
static void create_idle(void) {
    scr_idle = lv_obj_create(NULL);
    apply_screen_bg(scr_idle);
    make_header(scr_idle, "疲劳驾驶监测", &lv_font_chinese_18b);

    /* Clock card: big HH:MM:SS + glowing digits (montserrat_48) */
    lv_obj_t *card = make_card(scr_idle, 8, 52, 224, 104);

    idle_time = lv_label_create(card);
    lv_label_set_text(idle_time, "--:--:--");
    lv_obj_set_style_text_font(idle_time, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(idle_time, C_TXT, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(idle_time, C_ACCENT, LV_PART_MAIN);  /* blue glow halo */
    lv_obj_set_style_shadow_width(idle_time, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(idle_time, 0x70, LV_PART_MAIN);
    lv_obj_align(idle_time, LV_ALIGN_CENTER, 0, 0);

    /* Date + weekday on one line below the card (kept clear of status below) */
    idle_date = lv_label_create(scr_idle);
    lv_label_set_text(idle_date, "----年--月--日");
    lv_obj_set_style_text_font(idle_date, &lv_font_chinese_16b, LV_PART_MAIN);
    lv_obj_set_style_text_color(idle_date, C_TXT, LV_PART_MAIN);
    lv_obj_align(idle_date, LV_ALIGN_CENTER, -22, 18);

    idle_week = lv_label_create(scr_idle);
    lv_label_set_text(idle_week, "周二");
    lv_obj_set_style_text_font(idle_week, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(idle_week, C_TXT, LV_PART_MAIN);
    lv_obj_align(idle_week, LV_ALIGN_CENTER, 86, 18);   /* clear of the full-width date */

    idle_status = lv_label_create(scr_idle);
    lv_label_set_text(idle_status, "GPS:--\n摄像头:--");  /* no space (avoids box) */
    lv_obj_set_style_text_font(idle_status, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(idle_status, C_DIM, LV_PART_MAIN);
    lv_obj_align(idle_status, LV_ALIGN_CENTER, 0, 60);   /* one line below the date */

    idle_coord = lv_label_create(scr_idle);
    lv_label_set_text(idle_coord, "经度:--\n纬度:--");
    lv_obj_set_style_text_font(idle_coord, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(idle_coord, C_DIM, LV_PART_MAIN);
    lv_obj_align(idle_coord, LV_ALIGN_CENTER, 0, 94);

    lv_obj_t *hint = lv_label_create(scr_idle);
    lv_label_set_text(hint, "等待中...");
    lv_obj_set_style_text_font(hint, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, C_DIM, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
}

/* ── Identifying page ── */
static lv_obj_t *ident_label = nullptr;
static int ident_dots = 0;

static void create_identifying(void) {
    scr_identifying = lv_obj_create(NULL);
    apply_screen_bg(scr_identifying);
    make_header(scr_identifying, "人脸识别中", &lv_font_chinese_18b);

    lv_obj_t *card = make_card(scr_identifying, 40, 108, 160, 120);

    lv_obj_t *sp = lv_spinner_create(card);
    lv_obj_set_size(sp, 60, 60);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, -22);
    lv_obj_set_style_arc_color(sp, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 5, LV_PART_INDICATOR);

    ident_label = lv_label_create(card);
    lv_label_set_text(ident_label, "人脸识别中");
    lv_obj_set_style_text_font(ident_label, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(ident_label, C_TXT, LV_PART_MAIN);
    lv_obj_align(ident_label, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, "请正对摄像头");
    lv_obj_set_style_text_font(sub, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, C_DIM, LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 42);
}

/* ── Monitoring page ── */
static void create_monitoring(void) {
    scr_monitoring = lv_obj_create(NULL);
    apply_screen_bg(scr_monitoring);

    lv_obj_t *hdr = make_header(scr_monitoring, "疲劳监测", &lv_font_chinese_18b);
    lv_obj_t *chip = make_chip(hdr, "监测中", C_GOOD, &lv_font_chinese_16);
    lv_obj_align(chip, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ── PERCLOS arc gauge card (compact — leaves room for chart + info) ── */
    lv_obj_t *ga_card = make_card(scr_monitoring, 12, 46, 216, 74);
    lv_obj_t *arc = lv_arc_create(ga_card);
    lv_obj_set_size(arc, 64, 64);
    lv_obj_align(arc, LV_ALIGN_LEFT_MID, 12, 0);
    lv_arc_set_bg_angles(arc, 135, 45);          /* 270° sweep (classic gauge) */
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc, C_BORDER, LV_PART_MAIN);    /* track */
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, C_GOOD, LV_PART_INDICATOR); /* indicator (per level) */
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    mon_arc = arc;

    mon_pcl_v = lv_label_create(ga_card);        /* center % */
    lv_label_set_text(mon_pcl_v, "0%");
    lv_obj_set_style_text_font(mon_pcl_v, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(mon_pcl_v, C_TXT, LV_PART_MAIN);
    lv_obj_align(mon_pcl_v, LV_ALIGN_CENTER, -64, 0);   /* arc center is at x=44 */

    lv_obj_t *cap = lv_label_create(ga_card);    /* level caption */
    lv_label_set_text(cap, "疲劳");
    lv_obj_set_style_text_font(cap, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(cap, C_DIM, LV_PART_MAIN);
    lv_obj_set_pos(cap, 120, 22);

    mon_level = lv_label_create(ga_card);        /* level value (colored) */
    lv_label_set_text(mon_level, "正常");
    lv_obj_set_style_text_font(mon_level, &lv_font_chinese_16b, LV_PART_MAIN);
    lv_obj_set_style_text_color(mon_level, C_GOOD, LV_PART_MAIN);
    lv_obj_set_pos(mon_level, 120, 44);

    /* ── PERCLOS trend chart card (taller now) ── */
    lv_obj_t *ch_card = make_card(scr_monitoring, 12, 128, 216, 112);
    lv_obj_t *ctit = lv_label_create(ch_card);
    lv_label_set_text(ctit, "PERCLOS");
    lv_obj_set_style_text_font(ctit, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctit, C_DIM, LV_PART_MAIN);
    lv_obj_set_pos(ctit, 10, 6);

    mon_chart = lv_chart_create(ch_card);
    lv_obj_set_size(mon_chart, 196, 84);
    lv_obj_set_pos(mon_chart, 10, 22);
    lv_obj_set_style_bg_color(mon_chart, C_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mon_chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(mon_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mon_chart, 2, LV_PART_MAIN);
    lv_chart_set_type(mon_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(mon_chart, HIST);
    lv_chart_set_range(mon_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_range(mon_chart, LV_CHART_AXIS_PRIMARY_X, 0, HIST);
    lv_chart_set_div_line_count(mon_chart, 2, 0);
    lv_obj_set_style_line_color(mon_chart, C_BORDER, LV_PART_MAIN);  /* grid */
    lv_obj_set_style_line_width(mon_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(mon_chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(mon_chart, 0, 0, LV_PART_INDICATOR);  /* hide data points */

    mon_perclos_ser = lv_chart_add_series(mon_chart, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    mon_thresh30 = lv_chart_add_series(mon_chart, C_WARN, LV_CHART_AXIS_PRIMARY_Y);    /* mild */
    mon_thresh60 = lv_chart_add_series(mon_chart, C_BAD, LV_CHART_AXIS_PRIMARY_Y);     /* severe */
    for (int i = 0; i < HIST; i++) {
        lv_chart_set_next_value(mon_chart, mon_perclos_ser, 0);
        lv_chart_set_next_value(mon_chart, mon_thresh30, 15);
        lv_chart_set_next_value(mon_chart, mon_thresh60, 35);
    }

    /* ── 预测线 overlay: LVGL 9 chart 系列共享 LV_PART_ITEMS 线条样式,无法只让单个系列虚线。
     * 单独叠一个透明 chart(几何与 mon_chart 完全一致)只画预测系列,给它单独设虚线样式,
     * 这样虚线只作用于预测线,实线 PERCLOS/阈值线不受影响。 ── */
    mon_pred_chart = lv_chart_create(ch_card);
    lv_obj_set_size(mon_pred_chart, 196, 84);
    lv_obj_set_pos(mon_pred_chart, 10, 22);                    /* 与 mon_chart 完全重合 */
    lv_obj_set_style_bg_opa(mon_pred_chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(mon_pred_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mon_pred_chart, 2, LV_PART_MAIN);
    lv_obj_clear_flag(mon_pred_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mon_pred_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_chart_set_type(mon_pred_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(mon_pred_chart, HIST);
    lv_chart_set_range(mon_pred_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_range(mon_pred_chart, LV_CHART_AXIS_PRIMARY_X, 0, HIST);
    lv_chart_set_div_line_count(mon_pred_chart, 0, 0);         /* 无网格 */
    lv_obj_set_style_line_width(mon_pred_chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_line_dash_width(mon_pred_chart, 6, LV_PART_ITEMS);
    lv_obj_set_style_line_dash_gap(mon_pred_chart, 4, LV_PART_ITEMS);
    lv_obj_set_style_size(mon_pred_chart, 0, 0, LV_PART_INDICATOR);  /* 不画数据点 */
    mon_pred_ser = lv_chart_add_series(mon_pred_chart, C(0x5AB0FF), LV_CHART_AXIS_PRIMARY_Y); /* 浅蓝虚线 */
    for (int i = 0; i < HIST; i++)
        lv_chart_set_next_value(mon_pred_chart, mon_pred_ser, LV_CHART_POINT_NONE); /* 预测前隐藏 */

    /* ── Bottom info rows: [symbol][name][value] × 2 columns (explicit coords) ── */
    int y0 = 252, dy = 24;
    auto mkinfo = [&](int row, int sym_x, int name_x, int val_x,
                      const char *sym, const char *name, const lv_font_t *name_font,
                      lv_obj_t **val_out) {
        lv_obj_t *s = lv_label_create(scr_monitoring);
        lv_label_set_text(s, sym);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(s, C_ACCENT, LV_PART_MAIN);
        lv_obj_set_pos(s, sym_x, y0 + row * dy);
        lv_obj_t *n = lv_label_create(scr_monitoring);
        lv_label_set_text(n, name);
        lv_obj_set_style_text_font(n, name_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(n, C_DIM, LV_PART_MAIN);
        lv_obj_set_pos(n, name_x, y0 + row * dy);
        lv_obj_t *v = lv_label_create(scr_monitoring);
        lv_label_set_text(v, "--");
        lv_obj_set_style_text_font(v, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(v, C_TXT, LV_PART_MAIN);
        lv_obj_set_pos(v, val_x, y0 + row * dy);
        if (val_out) *val_out = v;
    };
    /* 左列值左移(96→84)；GPS 名称用 montserrat（半角，给右侧值腾位） */
    mkinfo(0, 14, 40, 84,    LV_SYMBOL_CHARGE,    "速度", &lv_font_chinese_16,   &mon_spd_v);    /* ⚡ */
    mkinfo(0, 156, 182, 216, LV_SYMBOL_GPS,       "GPS", &lv_font_montserrat_14, &mon_spd_gps);  /* ◎ */
    mkinfo(1, 14, 40, 84,    LV_SYMBOL_REFRESH,   "时长", &lv_font_chinese_16,   &mon_time_v);   /* ↻ */
    mkinfo(1, 156, 182, 222, LV_SYMBOL_SETTINGS,  "司机", &lv_font_chinese_16,   &mon_drv_num);  /* ⚙ */

    /* ── Yawn row (row 2) ── */
    {
        int ry = 2;
        lv_obj_t *s = lv_label_create(scr_monitoring);
        lv_label_set_text(s, LV_SYMBOL_WARNING);          /* ⚠ */
        lv_obj_set_style_text_font(s, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(s, C_WARN, LV_PART_MAIN);
        lv_obj_set_pos(s, 14, y0 + ry * dy);
        lv_obj_t *n = lv_label_create(scr_monitoring);
        lv_label_set_text(n, "哈欠");   /* 已加入字体（2026-08-05） */
        lv_obj_set_style_text_font(n, &lv_font_chinese_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(n, C_DIM, LV_PART_MAIN);
        lv_obj_set_pos(n, 40, y0 + ry * dy);
        mon_yawn_cnt = lv_label_create(scr_monitoring);
        lv_label_set_text(mon_yawn_cnt, "0 0%");
        lv_obj_set_style_text_font(mon_yawn_cnt, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(mon_yawn_cnt, C_TXT, LV_PART_MAIN);
        lv_obj_set_pos(mon_yawn_cnt, 78, y0 + ry * dy);

        lv_obj_t *nb = lv_label_create(scr_monitoring);
        lv_label_set_text(nb, "加成");   /* 已加入字体（2026-08-05） */
        lv_obj_set_style_text_font(nb, &lv_font_chinese_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(nb, C_DIM, LV_PART_MAIN);
        lv_obj_set_pos(nb, 138, y0 + ry * dy);
        mon_yawn_boost = lv_label_create(scr_monitoring);
        lv_label_set_text(mon_yawn_boost, "+0%");
        lv_obj_set_style_text_font(mon_yawn_boost, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(mon_yawn_boost, C_TXT, LV_PART_MAIN);
        lv_obj_set_pos(mon_yawn_boost, 176, y0 + ry * dy);
    }

    /* ── Prediction row (row 3) ── */
    {
        int ry = 3;
        lv_obj_t *s = lv_label_create(scr_monitoring);
        lv_label_set_text(s, LV_SYMBOL_WARNING);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(s, C_ACCENT, LV_PART_MAIN);
        lv_obj_set_pos(s, 14, y0 + ry * dy);
        lv_obj_t *n = lv_label_create(scr_monitoring);
        lv_label_set_text(n, "预测");
        lv_obj_set_style_text_font(n, &lv_font_chinese_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(n, C_DIM, LV_PART_MAIN);
        lv_obj_set_pos(n, 40, y0 + ry * dy);
        mon_pred = lv_label_create(scr_monitoring);
        lv_label_set_text(mon_pred, "--");
        lv_obj_set_style_text_font(mon_pred, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(mon_pred, C_TXT, LV_PART_MAIN);
        lv_obj_set_pos(mon_pred, 96, y0 + ry * dy);
    }
}

/* ── Rest page (休息中) ── */
static void create_rest(void) {
    scr_rest = lv_obj_create(NULL);
    apply_screen_bg(scr_rest);
    make_header(scr_rest, "请在服务区休息", &lv_font_chinese_16);

    /* Status chip: 强制休息(red) / 建议休息(yellow) */
    rest_status = lv_label_create(scr_rest);
    lv_label_set_text(rest_status, "休息中");
    lv_obj_set_style_text_font(rest_status, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(rest_status, C_GOOD, LV_PART_MAIN);
    lv_obj_set_width(rest_status, 200);
    lv_obj_align(rest_status, LV_ALIGN_TOP_MID, 0, 40);

    /* Countdown timer */
    rest_time_elapsed = lv_label_create(scr_rest);
    lv_label_set_text(rest_time_elapsed, "00:00");
    lv_obj_set_style_text_font(rest_time_elapsed, &lv_font_fancy_44, LV_PART_MAIN);
    lv_obj_set_style_text_color(rest_time_elapsed, C_ACCENT, LV_PART_MAIN);
    lv_obj_align(rest_time_elapsed, LV_ALIGN_CENTER, 0, -66);

    rest_time_total = lv_label_create(scr_rest);
    lv_label_set_text(rest_time_total, "/ 15:00");
    lv_obj_set_style_text_font(rest_time_total, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(rest_time_total, C_DIM, LV_PART_MAIN);
    lv_obj_align(rest_time_total, LV_ALIGN_CENTER, 0, -10);

    /* Progress bar */
    rest_bar = lv_bar_create(scr_rest);
    lv_obj_set_size(rest_bar, 200, 8);
    lv_obj_align(rest_bar, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_bg_color(rest_bar, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rest_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(rest_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(rest_bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(rest_bar, 0, 100);
    lv_bar_set_value(rest_bar, 0, LV_ANIM_OFF);

    /* Info card — only use chars known to be in lv_font_chinese_16 */
    lv_obj_t *card = make_card(scr_rest, 12, 158, 216, 104);
    int x0 = 14, dy = 26, y0 = 14;
    auto mkname = [&](int row, const char *t) {
        lv_obj_t *l = lv_label_create(card);
        lv_label_set_text(l, t);
        lv_obj_set_style_text_font(l, &lv_font_chinese_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, C_DIM, LV_PART_MAIN);
        lv_obj_set_pos(l, x0, y0 + row * dy);
        return l;
    };
    auto mkval = [&](int row, lv_obj_t **out) {
        lv_obj_t *l = lv_label_create(card);
        lv_label_set_text(l, "--");
        lv_obj_set_style_text_font(l, &lv_font_chinese_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, C_TXT, LV_PART_MAIN);
        lv_obj_set_pos(l, x0 + 90, y0 + row * dy);
        if (out) *out = l;
    };
    mkname(0, "驾驶时长:");  rest_drive = nullptr;      mkval(0, &rest_drive);
    mkname(1, "休息前疲劳:"); rest_pcl_before = nullptr; mkval(1, &rest_pcl_before);
    mkname(2, "停车位置:");   rest_loc = nullptr;        mkval(2, &rest_loc);

    /* Recovery status — simple text line */
    rest_recovery = lv_label_create(scr_rest);
    lv_label_set_text(rest_recovery, "");
    lv_obj_set_style_text_font(rest_recovery, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(rest_recovery, C_ACCENT, LV_PART_MAIN);
    lv_obj_align(rest_recovery, LV_ALIGN_BOTTOM_MID, 0, -40);

    /* Bottom hint */
    rest_hint = lv_label_create(scr_rest);
    lv_label_set_text(rest_hint, "");
    lv_obj_set_style_text_font(rest_hint, &lv_font_chinese_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(rest_hint, C_WARN, LV_PART_MAIN);
    lv_obj_align(rest_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

/* ── Screen switch ── */
static void do_switch(lv_obj_t *s) {
    if (scr_active == s || !s) return;
    lv_scr_load(s);  /* No animation — avoids black fade-in artifacts */
    scr_active = s;
}

static void check_switch(void) {
    bool id = g_display_data.identifying;
    bool cr = g_display_data.camera_ready;
    bool moving = g_display_data.gps_speed > 10.0f;
    bool rst = g_display_data.rest_active;
    if (rst == s_last_rest && id == s_last_id && cr == s_last_cr && moving == s_last_moving) return;
    ESP_LOGI(TAG, "Switch: cr=%d id=%d mv=%d rest=%d", cr, id, moving, rst);
    s_last_rest = rst; s_last_id = id; s_last_cr = cr; s_last_moving = moving;
    if (rst)          do_switch(scr_rest);          /* rest mode highest priority */
    else if (!cr)     do_switch(scr_idle);
    else if (id)      do_switch(scr_identifying);
    else if (!moving) do_switch(scr_idle);   /* stopped → standby page, no fatigue */
    else              do_switch(scr_monitoring);
}

/* ── Refresh (250ms) ── */
static void refresh_cb(lv_timer_t *t) {
    char b[128];
    check_switch();

    /* Animated dots on identifying page */
    if (scr_active == scr_identifying && ident_label) {
        ident_dots = (ident_dots + 1) % 4;
        if (ident_dots == 0) lv_label_set_text(ident_label, "人脸识别中");
        else if (ident_dots == 1) lv_label_set_text(ident_label, "人脸识别中.");
        else if (ident_dots == 2) lv_label_set_text(ident_label, "人脸识别中..");
        else lv_label_set_text(ident_label, "人脸识别中...");
    }

    /* Idle — update clock once per second (guarded): HH:MM:SS with blinking colons */
    {
        time_t now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);
        if (ti.tm_year >= 120 && ti.tm_sec != s_last_sec) {
            s_last_sec = ti.tm_sec;
            if (ti.tm_sec % 2 == 0)
                snprintf(b, sizeof(b), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
            else
                snprintf(b, sizeof(b), "%02d %02d %02d", ti.tm_hour, ti.tm_min, ti.tm_sec);  /* blink colons */
            lv_label_set_text(idle_time, b);
            snprintf(b, sizeof(b), "%04d年%02d月%02d日",
                     ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
            lv_label_set_text(idle_date, b);
            static const char *wd[7] = {"周日","周一","周二","周三","周四","周五","周六"};
            lv_label_set_text(idle_week, wd[ti.tm_wday]);
        }
    }
    snprintf(b, sizeof(b), "GPS:%s\n摄像头:%s",
             gps_status_str(),
             g_display_data.camera_ready ? "ON" : "--");
    lv_label_set_text(idle_status, b);
    if (g_display_data.gps_connected) {
        snprintf(b, sizeof(b), "经度:%.2f\n纬度:%.2f",
                 g_display_data.gps_lat, g_display_data.gps_lng);
    } else {
        snprintf(b, sizeof(b), "经度:--\n纬度:--");
    }
    lv_label_set_text(idle_coord, b);

    /* Monitoring — gauge + level + info rows */
    {
        float p = g_display_data.perclos;
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        snprintf(b, sizeof(b), "%d%%", (int)p);
        lv_label_set_text(mon_pcl_v, b);
        lv_arc_set_value(mon_arc, (int)p);
        lv_color_t lc = level_color(g_display_data.fatigue_level);
        lv_obj_set_style_arc_color(mon_arc, lc, LV_PART_INDICATOR);
        lv_label_set_text(mon_level, level_text(g_display_data.fatigue_level));
        lv_obj_set_style_text_color(mon_level, lc, LV_PART_MAIN);

        snprintf(b, sizeof(b), "%.0f km/h", g_display_data.gps_speed);
        lv_label_set_text(mon_spd_v, b);
        snprintf(b, sizeof(b), "%s", gps_status_str());
        lv_label_set_text(mon_spd_gps, b);
        /* GPS 状态着色: real=绿, mock=黄, no=灰 */
        lv_color_t gc = !g_display_data.gps_connected ? C_DIM :
                        (g_display_data.gps_mode == 0 ? C_GOOD : C_WARN);
        lv_obj_set_style_text_color(mon_spd_gps, gc, LV_PART_MAIN);

        uint32_t ds = g_display_data.gps_connected ? g_display_data.gps_drive_seconds
                     : g_display_data.driving_minutes * 60;
        int hh = ds / 3600, mm = (ds % 3600) / 60, ss = ds % 60;
        snprintf(b, sizeof(b), "%02d:%02d:%02d", hh, mm, ss);
        lv_label_set_text(mon_time_v, b);

        int dv = g_display_data.gps_connected ? g_display_data.gps_driver_id : g_display_data.driver_id;
        snprintf(b, sizeof(b), "%d", dv > 0 ? dv : 0);
        lv_label_set_text(mon_drv_num, b);
        lv_obj_set_style_text_color(mon_drv_num, lc, LV_PART_MAIN);  /* driver tinted by fatigue level */

        /* Yawn info: count + mouth dark-ratio% (diagnostic), boost */
        snprintf(b, sizeof(b), "%d %d%%", g_display_data.yawn_count,
                 (int)(g_display_data.mouth_dark_ratio * 100));
        lv_label_set_text(mon_yawn_cnt, b);
        snprintf(b, sizeof(b), "+%d%%", g_display_data.yawn_boost);
        lv_label_set_text(mon_yawn_boost, b);
        lv_color_t yc = g_display_data.yawn_active ? C_BAD : (g_display_data.yawn_boost > 0 ? C_WARN : C_DIM);
        lv_obj_set_style_text_color(mon_yawn_boost, yc, LV_PART_MAIN);

        /* Prediction */
        if (g_display_data.pred_minutes >= 0) {
            snprintf(b, sizeof(b), "%dmin", g_display_data.pred_minutes);
            lv_color_t pc = g_display_data.pred_minutes <= 5 ? C_BAD :
                            (g_display_data.pred_minutes <= 15 ? C_WARN : C_GOOD);
            lv_obj_set_style_text_color(mon_pred, pc, LV_PART_MAIN);
        } else if (g_display_data.history_count >= 4) {
            snprintf(b, sizeof(b), "平稳");
            lv_obj_set_style_text_color(mon_pred, C_GOOD, LV_PART_MAIN);
        } else {
            snprintf(b, sizeof(b), "--");
            lv_obj_set_style_text_color(mon_pred, C_DIM, LV_PART_MAIN);
        }
        lv_label_set_text(mon_pred, b);
    }

    /* Rest page — elapsed mm:ss, progress bar, snapshots */
    if (scr_active == scr_rest) {
        int es = g_display_data.rest_elapsed_seconds;
        int em = es / 60, es2 = es % 60;
        snprintf(b, sizeof(b), "%02d:%02d", em, es2);
        lv_label_set_text(rest_time_elapsed, b);
        int total = g_display_data.rest_total_minutes > 0 ? g_display_data.rest_total_minutes : 15;
        snprintf(b, sizeof(b), "/ %02d:00", total);
        lv_label_set_text(rest_time_total, b);

        /* Progress bar */
        int pct_bar = (total > 0) ? (es * 100 / (total * 60)) : 0;
        if (pct_bar > 100) pct_bar = 100;
        lv_bar_set_value(rest_bar, pct_bar, LV_ANIM_OFF);
        lv_color_t bar_c = g_display_data.rest_mandatory ? C_BAD :
                           (pct_bar > 90 ? C_GOOD : C_ACCENT);
        lv_obj_set_style_bg_color(rest_bar, bar_c, LV_PART_INDICATOR);

        /* Status label (no spaces: CJK font boxes on space/空格/~ 会变方框) */
        if (g_display_data.rest_recovery_pct >= 0) {
            lv_label_set_text(rest_status, "疲劳正常");
            lv_obj_set_style_text_color(rest_status, C_GOOD, LV_PART_MAIN);
        } else if (g_display_data.rest_mandatory) {
            lv_label_set_text(rest_status, "严重疲劳休息中");
            lv_obj_set_style_text_color(rest_status, C_BAD, LV_PART_MAIN);
        } else {
            lv_label_set_text(rest_status, "轻度疲劳休息中");
            lv_obj_set_style_text_color(rest_status, C_WARN, LV_PART_MAIN);
        }

        /* PERCLOS before (safe chars: 休,息,前) — value set in card row 1 */
        float snap = g_display_data.rest_snap_perclos;
        snprintf(b, sizeof(b), "%.0f%%", snap);
        lv_label_set_text(rest_pcl_before, b);

        /* Recovery — just number + % (no Chinese chars needed) */
        /* Recovery — time-based estimate during rest, real PERCLOS value after */
        int recov = g_display_data.rest_recovery_pct;
        if (recov == -2) {
            lv_label_set_text(rest_recovery, "手动退出");
            lv_obj_set_style_text_color(rest_recovery, C_DIM, LV_PART_MAIN);
        } else if (recov >= 0) {
            snprintf(b, sizeof(b), "%d%%", recov);
            lv_color_t rc = recov >= 70 ? C_GOOD : (recov >= 40 ? C_WARN : C_BAD);
            lv_obj_set_style_text_color(rest_recovery, rc, LV_PART_MAIN);
            lv_label_set_text(rest_recovery, b);
        } else if (g_display_data.rest_active) {
            /* Time-based recovery estimate while resting */
            int est = (total > 0) ? (es * 100 / (total * 60)) : 0;
            if (est > 100) est = 100;
            snprintf(b, sizeof(b), "%d%%", est);  /* 不用 ~: 中文字体无 ~ 会变方框 */
            lv_obj_set_style_text_color(rest_recovery, C_DIM, LV_PART_MAIN);
            lv_label_set_text(rest_recovery, b);
        } else {
            lv_label_set_text(rest_recovery, "");
        }

        /* Drive time */
        int dm = g_display_data.rest_snap_drive_min;
        snprintf(b, sizeof(b), "%d分钟", dm);
        lv_label_set_text(rest_drive, b);

        /* Location */
        lv_label_set_text(rest_loc, "服务区");

        /* Bottom hint (only use chars known in lv_font_chinese_16) */
        if (g_display_data.rest_mandatory && g_display_data.rest_recovery_pct < 0) {
            lv_label_set_text(rest_hint, "严重疲劳 请加时休息");
            lv_obj_set_style_text_color(rest_hint, C_BAD, LV_PART_MAIN);
        } else if (recov >= 0 && recov < 40) {
            snprintf(b, sizeof(b), "疲劳%d%% 请加时休息", recov);
            lv_label_set_text(rest_hint, b);
            lv_obj_set_style_text_color(rest_hint, C_WARN, LV_PART_MAIN);
        } else if (recov >= 70) {
            lv_label_set_text(rest_hint, "疲劳正常");
            lv_obj_set_style_text_color(rest_hint, C_GOOD, LV_PART_MAIN);
        } else if (recov >= 0) {
            snprintf(b, sizeof(b), "疲劳%d%%", recov);
            lv_label_set_text(rest_hint, b);
            lv_obj_set_style_text_color(rest_hint, C_WARN, LV_PART_MAIN);
        } else {
            lv_label_set_text(rest_hint, "");
        }
    }
}

/* ── PERCLOS sampler (2s) + prediction curve ── */
static int s_pt = 0;
static int32_t s_pred_array[HIST];
static void perclos_cb(lv_timer_t *t) {
    float v = g_display_data.perclos;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    lv_chart_set_next_value(mon_chart, mon_perclos_ser, v);

    /* Update prediction overlay (external array, stays aligned with chart).
     * perclos_cb 每 2s 采一点 → 每预测分钟占 30 个 chart 点。show=pred_n*30 让虚线
     * 从当前值一路画到阈值(右边缘),慢速 ramp 的 pred_n=1~3 时也占满 30~90 点,清晰可见;
     * 原 pred_n*2 在慢速 ramp 只画 2~6 点(≈几个像素),缩在右边缘几乎看不见。
     * 三态: ① 真实预测(pred_n>=0) ② 预热期(pred_ready=false)画默认斜率参考虚线,
     *        打开页面立即可见,数据够后自动切真实 ③ 平稳(pred_ready=true 且无上升)隐藏。 */
    float slope = g_display_data.perclos_slope;
    int pred_n = g_display_data.pred_minutes;
    int show;
    float step_slope;                          /* 用于投影的斜率 %/min */
    if (pred_n >= 0 && slope > 0.01f) {
        show = pred_n * 30;                    /* 30 chart pts / predicted minute */
        if (show < 12) show = 12;              /* 至少一段可见的短线 */
        if (show > HIST) show = HIST;
        step_slope = slope;
    } else if (!g_display_data.pred_ready) {
        show = 60;                             /* 预热期: 2 分钟参考投影 */
        step_slope = 3.0f;                     /* 默认 3%/min */
    } else {
        show = 0;                              /* 平稳/下降 → 不画 */
    }

    if (show > 0) {
        for (int i = 0; i < HIST; i++) {
            if (i < HIST - show) {
                s_pred_array[i] = (int32_t)LV_CHART_POINT_NONE;
            } else {
                int step = i - (HIST - show) + 1;
                float pred = v + step_slope * step * (1.0f / 30.0f);  /* 每点=2s=1/30min */
                if (pred < 0) pred = 0;
                if (pred > 100) pred = 100;
                s_pred_array[i] = (int32_t)pred;
            }
        }
    } else {
        for (int i = 0; i < HIST; i++)
            s_pred_array[i] = (int32_t)LV_CHART_POINT_NONE;
    }
    lv_chart_set_series_ext_y_array(mon_pred_chart, mon_pred_ser, s_pred_array);

    lv_chart_refresh(mon_chart);
    lv_chart_refresh(mon_pred_chart);
    if (++s_pt % 5 == 0)
        ESP_LOGI(TAG, "Chart: pt=%d perclos=%.1f pred=%dmin", s_pt, v, pred_n);
}

/* ── Init task ── */
void lvgl_ui_init_task(void) {
    lvgl_port_cfg_t c = ESP_LVGL_PORT_INIT_CONFIG();
    c.task_priority = 8; c.task_stack = 8192; c.task_affinity = 0; c.timer_period_ms = 5;
    /* MUST stay on internal RAM: spi_flash_disable_caches asserts every task
     * stack is in internal RAM (esp_task_stack_is_sane_cache_disabled).
     * Restored to 8KB since ESP-SR was disabled (frees ~73KB internal). */
    c.task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_ERROR_CHECK(lvgl_port_init(&c));
    ESP_LOGI(TAG, "LVGL task ok");
}

/* ── Init display ── */
void lvgl_ui_init_display(void) {
    ESP_LOGI(TAG, "Init display...");
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t pnl = screen_init_panel(&io);
    if (!pnl || !io) { ESP_LOGE(TAG, "Panel fail"); return; }

    lvgl_port_lock(0);
    const lvgl_port_display_cfg_t dc = {
        .io_handle = io, .panel_handle = pnl, .control_handle = NULL,
        .buffer_size = 240 * 320, .double_buffer = false, .trans_size = 0,
        .hres = 240, .vres = 320, .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .rounder_cb = NULL,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = false, .buff_spiram = true,
                   .sw_rotate = false, .swap_bytes = true,
                   .full_refresh = false, .direct_mode = false },
    };
    lv_display_t *disp = lvgl_port_add_disp(&dc);
    if (!disp) { ESP_LOGE(TAG, "Disp fail"); lvgl_port_unlock(); return; }
    lv_disp_set_default(disp);

    lv_obj_set_style_bg_color(lv_screen_active(), C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, LV_PART_MAIN);

    create_idle();
    create_identifying();
    create_monitoring();
    create_rest();
    do_switch(scr_idle);
    lv_timer_create(refresh_cb, 100, NULL);
    lv_timer_create(perclos_cb, 2000, NULL);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Display ready");
}
