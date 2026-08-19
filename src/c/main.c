#include <pebble.h>
#include "message_keys.auto.h"

// ---------- Globals ----------
static Window *s_main_window;
static Layer  *s_canvas_layer;

static GFont s_time_font;
static GFont s_date_font;
static GFont s_label_font;
static GFont s_small_font;

static int  s_batt_pct = 100;
static bool s_bt_connected = true;
static struct tm s_last_time;
static bool s_qt_active = false;
static bool s_prompt_handled_this_interval = false;

#if defined(PBL_HEALTH)
static int s_hr_bpm = 0;   // 0 = no reading yet / no sensor on this device
#endif

// ---------- Palette ----------
typedef enum {
  PALETTE_BLOOM  = 0,  // original pink/purple
  PALETTE_SAND  = 1,  // blue/teal
  PALETTE_FOREST = 2,  // green/sage
  PALETTE_FROST  = 3,  // icy white/slate
  PALETTE_EMBER  = 4   // dark charcoal/gold
} ColorPalette;

// ---------- Settings ----------
typedef enum {
  HAPTIC_FREQ_HOURLY         = 0,
  HAPTIC_FREQ_HALF_HOURLY    = 1,
  HAPTIC_FREQ_QUARTER_HOURLY = 2
} HapticFrequency;

typedef enum {
  BREATH_PATTERN_478 = 0,  // 4 in, 7 hold, 8 out (default)
  BREATH_PATTERN_55  = 1,  // 5 in, 5 out
  BREATH_PATTERN_BOX = 2   // 4 in, 4 hold, 4 out, 4 hold
} BreathPattern;

typedef struct {
  HapticFrequency haptic_frequency;
  ColorPalette    palette;
  BreathPattern   breath_pattern;
  bool            haptic_enabled;
} Settings;

static Settings s_settings;

// ---------- Breath tracking / UI state ----------
typedef enum {
  MODE_NORMAL = 0,
  MODE_PROMPT = 1,
  MODE_BREATH = 2
} UiMode;

typedef enum {
  PHASE_IN    = 0,
  PHASE_HOLD  = 1,
  PHASE_OUT   = 2,
  PHASE_HOLD2 = 3   // Box breath: hold after exhale
} BreathPhase;

static UiMode s_mode = MODE_NORMAL;

static int s_breath_count_today = 0;
static int s_last_yyyymmdd = 0;

static bool s_prompt_active = false;
static AppTimer *s_prompt_timer = NULL;

static AppTimer *s_breath_timer = NULL;
static BreathPhase s_phase = PHASE_IN;
static int s_phase_remaining = 0;

// Vibration-induced tap guard: the vibration motor mechanically contaminates the
// accelerometer and can register as a phantom tap. Ignore taps briefly after any
// vibe so the prompt's own buzz can't auto-start a breath sequence.
static bool s_ignore_taps = false;
static AppTimer *s_tap_guard_timer = NULL;

// ---------- Persistence keys ----------
enum {
  PERSIST_KEY_SETTINGS   = 1,
  PERSIST_KEY_BREATH_CNT = 2,
  PERSIST_KEY_LAST_DATE  = 3
};

// ---------- Utilities ----------
static int prv_date_yyyymmdd(const struct tm *t) {
  return (t->tm_year + 1900) * 10000 + (t->tm_mon + 1) * 100 + t->tm_mday;
}

static void prv_default_settings(void) {
  s_settings.haptic_frequency = HAPTIC_FREQ_QUARTER_HOURLY;
  s_settings.palette          = PALETTE_BLOOM;
  s_settings.breath_pattern   = BREATH_PATTERN_478;
  s_settings.haptic_enabled   = true;
}

static void prv_load_settings(void) {
  if (persist_exists(PERSIST_KEY_SETTINGS)) {
    persist_read_data(PERSIST_KEY_SETTINGS, &s_settings, sizeof(s_settings));

    int hf = (int)s_settings.haptic_frequency;
    if (hf < (int)HAPTIC_FREQ_HOURLY || hf > (int)HAPTIC_FREQ_QUARTER_HOURLY) {
      prv_default_settings();
      return;
    }

    int p = (int)s_settings.palette;
    if (p < (int)PALETTE_BLOOM || p > (int)PALETTE_EMBER) {
      s_settings.palette = PALETTE_BLOOM;
    }

    int bp = (int)s_settings.breath_pattern;
    if (bp < (int)BREATH_PATTERN_478 || bp > (int)BREATH_PATTERN_BOX) {
      s_settings.breath_pattern = BREATH_PATTERN_478;
    }

    // haptic_enabled is a bool — any value is valid, no range check needed
  } else {
    prv_default_settings();
  }
}

static void prv_save_settings(void) {
  persist_write_data(PERSIST_KEY_SETTINGS, &s_settings, sizeof(s_settings));
  if (s_canvas_layer) {
    layer_mark_dirty(s_canvas_layer);
  }
}

static void prv_load_breath_state(const struct tm *now_tm) {
  int today = prv_date_yyyymmdd(now_tm);

  if (persist_exists(PERSIST_KEY_LAST_DATE)) {
    s_last_yyyymmdd = persist_read_int(PERSIST_KEY_LAST_DATE);
  } else {
    s_last_yyyymmdd = today;
    persist_write_int(PERSIST_KEY_LAST_DATE, s_last_yyyymmdd);
  }

  if (persist_exists(PERSIST_KEY_BREATH_CNT)) {
    s_breath_count_today = persist_read_int(PERSIST_KEY_BREATH_CNT);
  } else {
    s_breath_count_today = 0;
    persist_write_int(PERSIST_KEY_BREATH_CNT, 0);
  }

  if (s_last_yyyymmdd != today) {
    s_last_yyyymmdd = today;
    s_breath_count_today = 0;
    persist_write_int(PERSIST_KEY_LAST_DATE, s_last_yyyymmdd);
    persist_write_int(PERSIST_KEY_BREATH_CNT, s_breath_count_today);
  }
}

static void prv_maybe_rollover_day(const struct tm *now_tm) {
  int today = prv_date_yyyymmdd(now_tm);
  if (today != s_last_yyyymmdd) {
    s_last_yyyymmdd = today;
    s_breath_count_today = 0;
    persist_write_int(PERSIST_KEY_LAST_DATE, s_last_yyyymmdd);
    persist_write_int(PERSIST_KEY_BREATH_CNT, s_breath_count_today);
  }
}

static bool prv_minute_matches_frequency(int minute) {
  switch (s_settings.haptic_frequency) {
    case HAPTIC_FREQ_HOURLY:         return minute == 0;
    case HAPTIC_FREQ_HALF_HOURLY:    return (minute == 0 || minute == 30);
    case HAPTIC_FREQ_QUARTER_HOURLY: return (minute % 15) == 0;
    default:                         return false;
  }
}

// ---------- Tap guard ----------
// Suppress accelerometer tap events for a short window whenever the vibration
// motor runs. The Pebble tap service can pick up motor vibration as a tap
// (accelerometer samples carry a did_vibrate flag for exactly this reason), so
// without this guard the prompt's own buzz self-triggers prv_start_breath_sequence().
static void prv_tap_guard_expire(void *data) {
  s_tap_guard_timer = NULL;
  s_ignore_taps = false;
}

static void prv_arm_tap_guard(uint32_t ms) {
  s_ignore_taps = true;
  if (s_tap_guard_timer) {
    app_timer_cancel(s_tap_guard_timer);
    s_tap_guard_timer = NULL;
  }
  s_tap_guard_timer = app_timer_register(ms, prv_tap_guard_expire, NULL);
}

// ---------- Vibes ----------
static void prv_gentle_vibe(void) {
  static const uint32_t segments[] = { 80, 60, 80 };
  VibePattern pat = {
    .durations    = segments,
    .num_segments = ARRAY_LENGTH(segments),
  };
  prv_arm_tap_guard(1000);   // motor pattern ~220ms + accel detection latency
  vibes_enqueue_custom_pattern(pat);
}

static void prv_tiny_confirm_vibe(void) {
  static const uint32_t segments[] = { 40 };
  VibePattern pat = {
    .durations    = segments,
    .num_segments = ARRAY_LENGTH(segments),
  };
  prv_arm_tap_guard(1000);
  vibes_enqueue_custom_pattern(pat);
}

// ---------- Prompt / Breath sequence ----------
static void prv_prompt_timeout(void *data) {
  s_prompt_timer  = NULL;
  s_prompt_active = false;
  if (s_mode == MODE_PROMPT) {
    s_mode = MODE_NORMAL;
    layer_mark_dirty(s_canvas_layer);
  }
}

static void prv_show_prompt(void) {
  s_mode          = MODE_PROMPT;
  s_prompt_active = true;

  if (s_prompt_timer) {
    app_timer_cancel(s_prompt_timer);
    s_prompt_timer = NULL;
  }
  s_prompt_timer = app_timer_register(55000, prv_prompt_timeout, NULL);
  layer_mark_dirty(s_canvas_layer);
}

static void prv_breath_tick(void *data);

static void prv_start_breath_sequence(void) {
  if (s_prompt_timer) {
    app_timer_cancel(s_prompt_timer);
    s_prompt_timer = NULL;
  }
  s_prompt_active   = false;
  s_mode            = MODE_BREATH;
  s_phase           = PHASE_IN;

  // Initial inhale duration depends on pattern
  switch (s_settings.breath_pattern) {
    case BREATH_PATTERN_55:  s_phase_remaining = 5; break;
    case BREATH_PATTERN_BOX: s_phase_remaining = 4; break;
    default:                 s_phase_remaining = 4; break; // 4-7-8
  }

  if (s_breath_timer) {
    app_timer_cancel(s_breath_timer);
    s_breath_timer = NULL;
  }
  s_breath_timer = app_timer_register(1000, prv_breath_tick, NULL);

  if (s_settings.haptic_enabled && !quiet_time_is_active()) prv_tiny_confirm_vibe();
  layer_mark_dirty(s_canvas_layer);
}

static void prv_complete_breath_sequence(void) {
  s_breath_count_today++;
  persist_write_int(PERSIST_KEY_BREATH_CNT, s_breath_count_today);

  if (s_settings.haptic_enabled && !quiet_time_is_active()) prv_gentle_vibe();

  s_mode         = MODE_NORMAL;
  s_breath_timer = NULL;
  layer_mark_dirty(s_canvas_layer);
}

static void prv_breath_tick(void *data) {
  s_breath_timer = NULL;
  s_phase_remaining--;

  if (s_phase_remaining <= 0) {
    switch (s_settings.breath_pattern) {

      case BREATH_PATTERN_55:
        if (s_phase == PHASE_IN) {
          s_phase = PHASE_OUT; s_phase_remaining = 5;
        } else {
          prv_complete_breath_sequence(); return;
        }
        break;

      case BREATH_PATTERN_BOX:
        if (s_phase == PHASE_IN) {
          s_phase = PHASE_HOLD;  s_phase_remaining = 4;
        } else if (s_phase == PHASE_HOLD) {
          s_phase = PHASE_OUT;   s_phase_remaining = 4;
        } else if (s_phase == PHASE_OUT) {
          s_phase = PHASE_HOLD2; s_phase_remaining = 4;
        } else {
          // PHASE_HOLD2 complete
          prv_complete_breath_sequence(); return;
        }
        break;

      default: // BREATH_PATTERN_478
        if (s_phase == PHASE_IN) {
          s_phase = PHASE_HOLD; s_phase_remaining = 7;
        } else if (s_phase == PHASE_HOLD) {
          s_phase = PHASE_OUT;  s_phase_remaining = 8;
        } else {
          prv_complete_breath_sequence(); return;
        }
        break;
    }
  }

  layer_mark_dirty(s_canvas_layer);
  s_breath_timer = app_timer_register(1000, prv_breath_tick, NULL);
}

// ---------- AppMessage ----------
static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  // Haptic frequency
  Tuple *freq_t = dict_find(iter, MESSAGE_KEY_HapticFrequency);
  if (freq_t) {
    int v;
    if (freq_t->type == TUPLE_CSTRING) {
      v = atoi(freq_t->value->cstring);
    } else {
      v = (int)freq_t->value->int32;
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "Received HapticFrequency=%d", v);
    if (v >= (int)HAPTIC_FREQ_HOURLY && v <= (int)HAPTIC_FREQ_QUARTER_HOURLY) {
      s_settings.haptic_frequency = (HapticFrequency)v;
    }
  }

  // Palette
  Tuple *pal_t = dict_find(iter, MESSAGE_KEY_Palette);
  if (pal_t) {
    int v;
    if (pal_t->type == TUPLE_CSTRING) {
      v = atoi(pal_t->value->cstring);
    } else {
      v = (int)pal_t->value->int32;
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "Received Palette=%d", v);
    if (v >= (int)PALETTE_BLOOM && v <= (int)PALETTE_EMBER) {
      s_settings.palette = (ColorPalette)v;
    }
  }

  // Breath pattern
  Tuple *bp_t = dict_find(iter, MESSAGE_KEY_BreathPattern);
  if (bp_t) {
    int v;
    if (bp_t->type == TUPLE_CSTRING) {
      v = atoi(bp_t->value->cstring);
    } else {
      v = (int)bp_t->value->int32;
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "Received BreathPattern=%d", v);
    if (v >= (int)BREATH_PATTERN_478 && v <= (int)BREATH_PATTERN_BOX) {
      s_settings.breath_pattern = (BreathPattern)v;
    }
  }

  // Haptic enabled toggle
  Tuple *he_t = dict_find(iter, MESSAGE_KEY_HapticEnabled);
  if (he_t) {
    s_settings.haptic_enabled = (bool)he_t->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "Received HapticEnabled=%d", (int)s_settings.haptic_enabled);
  }

  prv_save_settings();
}

// ---------- Color helpers ----------

// Each palette exposes: bg, accent, petal_light, petal_mid, petal_dark,
//                       petal_center, leaf, leaf_dark, outline

static inline GColor color_bg(void) {
#if defined(PBL_COLOR)
  switch (s_settings.palette) {
    case PALETTE_SAND:  return GColorWhite;
    case PALETTE_FOREST: return GColorWhite;
    case PALETTE_FROST:  return GColorFromRGB(85, 170, 170);
    case PALETTE_EMBER:  return GColorFromRGB( 30,  25,  20);
    default:             return GColorWhite;                   // BLOOM
  }
#else
  return GColorWhite;
#endif
}

static inline GColor color_accent(void) {
#if defined(PBL_COLOR)
  switch (s_settings.palette) {
    case PALETTE_SAND:  return GColorFromRGB(170, 85, 0);
    case PALETTE_FOREST: return GColorFromRGB(85, 85, 0);
    case PALETTE_FROST:  return GColorFromRGB(0, 0, 85);
    case PALETTE_EMBER:  return GColorFromRGB(220, 120,  30);
    default:             return GColorFromRGB( 88,  43, 128); // BLOOM purple
  }
#else
  return GColorBlack;
#endif
}

static inline GColor color_outline(void) {
#if defined(PBL_COLOR)
  switch (s_settings.palette) {
    case PALETTE_SAND:  return GColorFromRGB(85, 85, 0);
    case PALETTE_FOREST: return GColorFromRGB(85, 85, 0);
    case PALETTE_FROST:  return GColorFromRGB(0, 0, 85);
    case PALETTE_EMBER:  return GColorFromRGB(160,  90,  10);
    default:             return GColorFromRGB( 80,  30, 130); // BLOOM
  }
#else
  return GColorBlack;
#endif
}

static inline GColor color_petal_light(void) {
#if defined(PBL_COLOR)
  switch (s_settings.palette) {
    case PALETTE_SAND:  return GColorFromRGB(255, 255, 170);
    case PALETTE_FOREST: return GColorFromRGB(170, 170, 85);
    case PALETTE_FROST:  return GColorFromRGB(85, 170, 255);
    case PALETTE_EMBER:  return GColorFromRGB(255, 210, 100);
    default:             return GColorFromRGB(255, 140, 200); // BLOOM light pink
  }
#else
  return GColorLightGray;
#endif
}

static inline GColor color_petal_mid(void) {
#if defined(PBL_COLOR)
  switch (s_settings.palette) {
    case PALETTE_SAND:  return GColorFromRGB(255, 170, 85);
    case PALETTE_FOREST: return GColorFromRGB(85, 85, 0);
    case PALETTE_FROST:  return GColorFromRGB(170, 170, 170);
    case PALETTE_EMBER:  return GColorFromRGB(220, 120,  30);
    default:             return GColorFromRGB(235,  90, 170); // BLOOM mid pink
  }
#else
  return GColorDarkGray;
#endif
}

static inline GColor color_petal_dark(void) {
#if defined(PBL_COLOR)
  switch (s_settings.palette) {
    case PALETTE_SAND:  return GColorFromRGB(170, 85, 0);
    case PALETTE_FOREST: return GColorFromRGB(0, 85, 0);
    case PALETTE_FROST:  return GColorFromRGB(245, 248, 252);
    case PALETTE_EMBER:  return GColorFromRGB(190,  70,  10);
    default:             return GColorFromRGB(180,  70, 150); // BLOOM dark pink
  }
#else
  return GColorBlack;
#endif
}

static inline GColor color_petal_center(void) {
#if defined(PBL_COLOR)
  switch (s_settings.palette) {
    case PALETTE_SAND:  return GColorFromRGB(85, 85, 0);
    case PALETTE_FOREST: return GColorFromRGB(85, 0, 0);
    case PALETTE_FROST:  return GColorFromRGB(0, 0, 85);
    case PALETTE_EMBER:  return GColorFromRGB(0, 0, 85);
    default:             return GColorFromRGB(120,  40, 120); // BLOOM
  }
#else
  return GColorBlack;
#endif
}

static inline GColor color_leaf(void) {
#if defined(PBL_COLOR)
  switch (s_settings.palette) {
    case PALETTE_SAND:  return GColorFromRGB(170, 170, 85);
    case PALETTE_FOREST: return GColorFromRGB(0, 85, 0);
    case PALETTE_FROST:  return GColorFromRGB(80, 175, 170);
    case PALETTE_EMBER:  return GColorFromRGB(100,  70,  30);
    default:             return GColorFromRGB( 40, 200,  90); // BLOOM green
  }
#else
  return GColorDarkGray;
#endif
}

static inline GColor color_leaf_dark(void) {
#if defined(PBL_COLOR)
  switch (s_settings.palette) {
    case PALETTE_SAND:  return GColorFromRGB(85, 85, 0);
    case PALETTE_FOREST: return GColorFromRGB(0, 85, 0);
    case PALETTE_FROST:  return GColorFromRGB(245, 248, 252);
    case PALETTE_EMBER:  return GColorFromRGB( 60,  40,  10);
    default:             return GColorFromRGB( 20, 140,  70); // BLOOM
  }
#else
  return GColorBlack;
#endif
}

// ---------- Lotus geometry ----------
static int openness_for_breaths(int breaths_today) {
  if (breaths_today <= 0) return 0;

  int denom;
  switch (s_settings.haptic_frequency) {
    case HAPTIC_FREQ_QUARTER_HOURLY: denom = 24; break;
    case HAPTIC_FREQ_HALF_HOURLY:    denom = 12; break;
    case HAPTIC_FREQ_HOURLY:         denom =  6; break;
    default:                         denom = 24; break;
  }

  if (breaths_today >= denom) return 100;
  return (breaths_today * 100) / denom;
}

static void draw_triangle(GContext *ctx, GPoint p0, GPoint p1, GPoint p2, GColor fill, GColor edge) {
  GPoint pts[3] = { p0, p1, p2 };
  GPathInfo info = { .num_points = 3, .points = pts };

  GPath *path = gpath_create(&info);
  graphics_context_set_fill_color(ctx, fill);
  gpath_draw_filled(ctx, path);
  graphics_context_set_stroke_color(ctx, edge);
  gpath_draw_outline(ctx, path);
  gpath_destroy(path);
}

static void draw_lotus_open(GContext *ctx, int cx, int cy, int o /*0..100*/) {
  if (o < 0) o = 0;
  if (o > 100) o = 100;

  const int backBase  = 58;
  const int backBaseY = cy + 30;
  const int backTipY  = cy - 18;
  const int backLift  = (o * 10) / 100;

  const int sideBaseOutX = 58;
  const int sideBaseY    = cy + 35;
  const int sideFootX    = 6;
  const int sideFootY    = cy + 44;
  const int sideTipBaseX = 30;
  const int sideTipOutX  = sideTipBaseX + (o * 26) / 100;
  const int sideTipBaseY = cy + 8;
  const int sideTipUp    = (o * 12) / 100;

  const int centerBase  = 22;
  const int centerBaseY = cy + 38;
  const int centerTipY  = cy + 10;
  const int centerTipUp = (o * 10) / 100;

  const int frontBaseY    = cy + 44;
  const int frontInnerGap = 2;
  const int frontOuterX   = 40;
  const int frontTipY     = cy + 12;
  const int frontTipUp    = (o * 10) / 100;
  const int frontTipOut   = (o * 22) / 100;

  const int leafW = 62;
  const int leafH = 12;
  const int leafY = cy + 28;

  const GColor outline = color_outline();

  // Leaves
  graphics_context_set_fill_color(ctx, color_leaf());
  graphics_fill_rect(ctx, GRect(cx - leafW - 8, leafY, leafW, leafH), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(cx + 8,         leafY, leafW, leafH), 0, GCornerNone);

  graphics_context_set_fill_color(ctx, color_leaf_dark());
  graphics_fill_rect(ctx, GRect(cx - leafW - 8, leafY,             leafW, 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(cx - leafW - 8, leafY + leafH - 2, leafW, 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(cx + 8,         leafY,             leafW, 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(cx + 8,         leafY + leafH - 2, leafW, 2), 0, GCornerNone);

  // Back petal
  draw_triangle(ctx,
    GPoint(cx - backBase, backBaseY),
    GPoint(cx,            backTipY - backLift),
    GPoint(cx + backBase, backBaseY),
    color_petal_light(), outline);

  // Side petals
  draw_triangle(ctx,
    GPoint(cx - sideBaseOutX, sideBaseY),
    GPoint(cx - sideTipOutX,  sideTipBaseY - sideTipUp),
    GPoint(cx + sideFootX,    sideFootY),
    color_petal_mid(), outline);

  draw_triangle(ctx,
    GPoint(cx + sideBaseOutX, sideBaseY),
    GPoint(cx + sideTipOutX,  sideTipBaseY - sideTipUp),
    GPoint(cx - sideFootX,    sideFootY),
    color_petal_mid(), outline);

  // Center petal
  draw_triangle(ctx,
    GPoint(cx - centerBase, centerBaseY),
    GPoint(cx,              centerTipY - centerTipUp),
    GPoint(cx + centerBase, centerBaseY),
    color_petal_center(), outline);

#if defined(PBL_COLOR)
  // Yellow center stamen — rises as lotus opens
  if (o > 25) {
    int yo   = (o - 25);
    int h    = 4 + (yo * 10) / 75;
    int w    = 3 + (yo * 8)  / 75;
    int baseY = cy + 38 - (yo * 12) / 75;
    int tipY  = baseY - h;

    GColor stamen;
    switch (s_settings.palette) {
      case PALETTE_SAND:  stamen = GColorYellow;               break;
      case PALETTE_FOREST:stamen = GColorFromRGB(255, 170, 0); break;
      case PALETTE_FROST:stamen = GColorFromRGB(255, 170, 0); break;
      case PALETTE_EMBER:  stamen = GColorYellow;  break;
      default:             stamen = GColorYellow;               break;
    }
    draw_triangle(ctx,
      GPoint(cx - w, baseY),
      GPoint(cx,     tipY),
      GPoint(cx + w, baseY),
      stamen, outline);
  }
#endif

  // Front split petals
  draw_triangle(ctx,
    GPoint(cx - frontOuterX,       frontBaseY),
    GPoint(cx - (frontTipOut + 6), frontTipY - frontTipUp),
    GPoint(cx - frontInnerGap,     frontBaseY),
    color_petal_dark(), outline);

  draw_triangle(ctx,
    GPoint(cx + frontInnerGap,     frontBaseY),
    GPoint(cx + (frontTipOut + 6), frontTipY - frontTipUp),
    GPoint(cx + frontOuterX,       frontBaseY),
    color_petal_dark(), outline);
}

// ---------- Text helpers ----------
static void draw_text_center(GContext *ctx, const char *text, GFont font, GColor color, int y) {
  GRect bounds = layer_get_bounds(s_canvas_layer);
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(
    ctx, text, font,
    GRect(0, y, bounds.size.w, 50),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentCenter,
    NULL
  );
}

// ---------- Quiet Time ----------
static void prv_update_qt_and_redraw(void) {
  bool qt = quiet_time_is_active();
  if (qt != s_qt_active) {
    s_qt_active = qt;
    layer_mark_dirty(s_canvas_layer);
  }
}

// ---------- BT icon ----------
// Stylized Bluetooth rune with a disconnect slash. Centered in the slot at slot_x.
static void draw_bt_off_icon(GContext *ctx, int slot_x, int slot_w, int bt_y, GColor color) {
  const int sx = slot_x + slot_w / 2;   // spine center
  const int ty = bt_y + 3;
  const int by = bt_y + 17;
  const int q1 = bt_y + 6;
  const int q3 = bt_y + 14;
  const int hw = 4;
  const int rx = sx + hw;
  const int lx = sx - hw;

  graphics_context_set_stroke_color(ctx, color);

  // Rune: spine + two right triangles, formed by two diagonals crossing the spine
  graphics_draw_line(ctx, GPoint(sx, ty), GPoint(sx, by));   // spine
  graphics_draw_line(ctx, GPoint(sx, ty), GPoint(rx, q1));   // top -> upper-right knee
  graphics_draw_line(ctx, GPoint(rx, q3), GPoint(sx, by));   // lower-right knee -> bottom
  graphics_draw_line(ctx, GPoint(rx, q1), GPoint(lx, q3));   // crosses center to lower-left
  graphics_draw_line(ctx, GPoint(lx, q1), GPoint(rx, q3));   // crosses center to lower-right

  // Disconnect slash (2px for visibility)
  graphics_draw_line(ctx, GPoint(rx + 2, ty - 1), GPoint(lx - 2, by + 1));
  graphics_draw_line(ctx, GPoint(rx + 1, ty - 1), GPoint(lx - 3, by + 1));
}

// ---------- QT icon ----------
// Muted speaker: body box + cone horn with a diagonal slash. Centered in the
// slot at slot_x. row_y is the top of the indicator row (slot is ~20 tall).
static void draw_qt_mute_icon(GContext *ctx, int slot_x, int slot_w, int row_y, GColor color) {
  const int cxs = slot_x + slot_w / 2;   // slot center x
  const int sx  = cxs - 6;               // left edge of the speaker body
  const int vy  = row_y + 10;            // vertical center of the row

  graphics_context_set_fill_color(ctx, color);

  // Speaker body (small box)
  graphics_fill_rect(ctx, GRect(sx, vy - 3, 4, 6), 0, GCornerNone);

  // Cone (trapezoid horn, flares out to the right; left edge flush with body)
  GPoint cone[4] = {
    GPoint(sx + 4,  vy - 3),
    GPoint(sx + 12, vy - 7),
    GPoint(sx + 12, vy + 7),
    GPoint(sx + 4,  vy + 3),
  };
  GPathInfo cone_info = { .num_points = 4, .points = cone };
  GPath *cone_path = gpath_create(&cone_info);
  gpath_draw_filled(ctx, cone_path);
  gpath_destroy(cone_path);

  // Mute slash (2px), top-right to bottom-left across the whole icon
  graphics_context_set_stroke_color(ctx, color);
  graphics_draw_line(ctx, GPoint(sx + 14, vy - 8), GPoint(sx - 2, vy + 8));
  graphics_draw_line(ctx, GPoint(sx + 13, vy - 8), GPoint(sx - 3, vy + 8));
}


// ---------- Bottom indicators ----------
static void draw_bottom_indicators(GContext *ctx) {
  GRect bounds = layer_get_bounds(s_canvas_layer);

  char batt[8];
  snprintf(batt, sizeof(batt), "%d%%", s_batt_pct);
  char cnt[16];
  snprintf(cnt, sizeof(cnt), "%d", s_breath_count_today);

  GColor accent = color_accent();
  graphics_context_set_text_color(ctx, accent);

#if defined(PBL_ROUND)
  const int row_y  = bounds.size.h - 60;
  const int inset  = 38;
  const int row_w  = bounds.size.w - (inset * 2);

  graphics_draw_text(ctx, batt, s_small_font,
                     GRect(inset, row_y, 72, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  graphics_draw_text(ctx, cnt, s_small_font,
                     GRect(inset + 60, row_y, row_w - 120, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  const int bt_w_r = 25;
  const int bt_x_r = bounds.size.w - inset - bt_w_r;

  if (s_qt_active) {
    const int qt_w = 22;
    const int gap  = 2;
    draw_qt_mute_icon(ctx, bt_x_r - gap - qt_w, qt_w, row_y, accent);
  }
  if (!s_bt_connected) {
    draw_bt_off_icon(ctx, bt_x_r, bt_w_r, row_y, accent);
  }

#else
  const int padding = 8;
  const int bt_w = 25;
  const int bt_x = bounds.size.w - bt_w - padding;
  const int bt_y = bounds.size.h - 26;

  graphics_draw_text(ctx, batt, s_small_font,
                     GRect(padding, bt_y, 60, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

#if defined(PBL_HEALTH)
  if (s_hr_bpm > 0) {
    char hr[12];
    snprintf(hr, sizeof(hr), "%d", s_hr_bpm);
    graphics_draw_text(ctx, hr, s_small_font,
                       GRect(bounds.size.w - padding - 28, bt_y, 28, 20),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  }
#endif

  graphics_draw_text(ctx, cnt, s_small_font,
                     GRect(60, bt_y, bounds.size.w - 120, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  if (s_qt_active) {
    const int qt_w = 22;
    const int gap  = 2;
    draw_qt_mute_icon(ctx, bt_x - gap - qt_w, qt_w, bt_y, accent);
  }
  if (!s_bt_connected) {
    draw_bt_off_icon(ctx, 36, 60, bt_y, accent);
  }
#endif
}

// ---------- Main draw ----------
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, color_bg());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  const int cx = bounds.size.w / 2;
  const int cy = (bounds.size.h / 2) - 35;

  int o = openness_for_breaths(s_breath_count_today);
   //o = 100;  // TEMP: force full bloom for screenshot
  draw_lotus_open(ctx, cx, cy, o);

  GColor accent = color_accent();

  if (s_mode == MODE_PROMPT) {
    draw_text_center(ctx, "Time for a Breath Break", s_label_font, accent, PBL_IF_ROUND_ELSE(24, 1));
  } else if (s_mode == MODE_BREATH) {
    const char *phase_text =
      (s_phase == PHASE_IN)                             ? "Breathe In"  :
      (s_phase == PHASE_OUT)                            ? "Breathe Out" :
      (s_phase == PHASE_HOLD || s_phase == PHASE_HOLD2) ? "Hold"        : "";
    draw_text_center(ctx, phase_text, s_label_font, accent, PBL_IF_ROUND_ELSE(24, 1));

    static char cd[8];
    snprintf(cd, sizeof(cd), "%ds", s_phase_remaining);
    draw_text_center(ctx, cd, s_date_font, accent, 26);
  } else {
    draw_text_center(ctx, "Breathe", s_label_font, accent, PBL_IF_ROUND_ELSE(24, 1));
  }

  static char time_str[6];
  strftime(time_str, sizeof(time_str),
           clock_is_24h_style() ? "%H:%M" : "%I:%M",
           &s_last_time);
  draw_text_center(ctx, time_str, s_time_font, accent, (bounds.size.h / 2) + 10);

  static char date_str[16];
  strftime(date_str, sizeof(date_str), "%a %b %d", &s_last_time);
  draw_text_center(ctx, date_str, s_date_font, accent, (bounds.size.h / 2) + 5 + 42 - 6);

  draw_bottom_indicators(ctx);
}

// ---------- Click handling ----------
static void tap_handler(AccelAxisType axis, int32_t direction) {
  if (s_ignore_taps) return;   // ignore vibration-induced phantom taps
  if (s_mode == MODE_PROMPT) {
    prv_start_breath_sequence();
    s_prompt_handled_this_interval = true;
  }
}

static void click_config_provider(void *context) {
  // Intentionally blank
}

// ---------- Services ----------
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  s_last_time = *tick_time;
  prv_maybe_rollover_day(tick_time);

  if (prv_minute_matches_frequency(tick_time->tm_min)) {
    if (!s_prompt_handled_this_interval) {
      if (s_settings.haptic_enabled && !quiet_time_is_active()) {
        prv_gentle_vibe();
      }
      prv_show_prompt();
    }
  } else {
    s_prompt_handled_this_interval = false;  // reset for next interval
  }

  prv_update_qt_and_redraw();
  layer_mark_dirty(s_canvas_layer);
}

static void battery_handler(BatteryChargeState state) {
  s_batt_pct = state.charge_percent;
  layer_mark_dirty(s_canvas_layer);
}

static void bt_handler(bool connected) {
  s_bt_connected = connected;
  layer_mark_dirty(s_canvas_layer);
}

#if defined(PBL_HEALTH)
static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventHeartRateUpdate ||
      event == HealthEventSignificantUpdate) {
    HealthValue v = health_service_peek_current_value(HealthMetricHeartRateBPM);
    s_hr_bpm = (v > 0) ? (int)v : 0;
    if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
  }
}
#endif

// ---------- Window lifecycle ----------
static void main_window_load(Window *window) {
  window_set_click_config_provider(window, click_config_provider);

  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(root, s_canvas_layer);

  s_time_font  = fonts_get_system_font(FONT_KEY_BITHAM_34_MEDIUM_NUMBERS);
  s_date_font  = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_label_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_small_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
}

static void main_window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
}

// ---------- Init / Deinit ----------
static void init(void) {
  prv_load_settings();

  s_main_window = window_create();

  accel_tap_service_subscribe(tap_handler);

#if defined(PBL_COLOR)
  window_set_background_color(s_main_window, color_bg());
#endif

  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load   = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);

  time_t now   = time(NULL);
  s_last_time  = *localtime(&now);

  prv_load_breath_state(&s_last_time);

  app_message_register_inbox_received(inbox_received_callback);
  app_message_open(128, 64);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  connection_service_subscribe((ConnectionHandlers){
    .pebble_app_connection_handler = bt_handler
  });

#if defined(PBL_HEALTH)
  health_service_events_subscribe(health_handler, NULL);
  HealthValue hr0 = health_service_peek_current_value(HealthMetricHeartRateBPM);
  if (hr0 > 0) s_hr_bpm = (int)hr0;   // seed if a reading already exists
#endif

  battery_handler(battery_state_service_peek());
  s_bt_connected = connection_service_peek_pebble_app_connection();
}

static void deinit(void) {
  if (s_prompt_timer) {
    app_timer_cancel(s_prompt_timer);
    s_prompt_timer = NULL;
  }
  if (s_breath_timer) {
    app_timer_cancel(s_breath_timer);
    s_breath_timer = NULL;
  }
  if (s_tap_guard_timer) {
    app_timer_cancel(s_tap_guard_timer);
    s_tap_guard_timer = NULL;
  }

  connection_service_unsubscribe();
  battery_state_service_unsubscribe();
  tick_timer_service_unsubscribe();
  accel_tap_service_unsubscribe();

#if defined(PBL_HEALTH)
  health_service_events_unsubscribe();
#endif

  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}