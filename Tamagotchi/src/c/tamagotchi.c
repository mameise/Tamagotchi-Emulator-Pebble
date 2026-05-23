#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define FPS 30
#define FPS_DELAY 1000/FPS //ms
#define STEP_DELAY 1 //ms
#define STEPS_PER_DELAY 600//55  

#define VRAM_SIZE (64 + 13)
#define BYTES_PER_LINE 32
#define BITS_PER_BYTE 8

//#undef PBL_COLOR // only used for testing B&W

#include <pebble.h>
#include "tamalib/tamalib.h"
#include "tama_rtc_sync.h"
//#include "rom.h" 

static void initTamalib(void); 
static void saveCurrentState(bool isAutoSave);
static void saveCurrentStateAndQuit();
static void autosave_timer_callback(void *data);
static void rtc_sync_timer_callback(void *data);
static bool persistSaveState(void);
static bool persistLoadState(void);
static bool loadRomFromResource(void);
static void loadSettingsFromPersist(void);
static void autosave_timer_callback(void *data);

static Window *s_main_window;
static BitmapLayer *s_background_layer;
static Layer *s_screen_layer;
static Layer *s_icons_layer;
static TextLayer *s_text_layer;
static TextLayer *s_battery_layer = NULL;
static char s_battery_text[8] = "";
static TextLayer *s_time_layer = NULL;
static char s_time_text[8] = "";
static TextLayer *s_date_layer = NULL;
static char s_date_text[16] = "";
//static GFont s_lcd_font;

// Bitmaps
static GBitmap *s_bitmap_bg;
static GBitmap *s_bitmap_icon1;
static GBitmap *s_bitmap_icon2;
static GBitmap *s_bitmap_icon3;
static GBitmap *s_bitmap_icon4;
static GBitmap *s_bitmap_icon5;
static GBitmap *s_bitmap_icon6;
static GBitmap *s_bitmap_icon7;
static GBitmap *s_bitmap_icon8;

//static const uint8_t LCD_WIDTH  = 32;
//static const uint8_t LCD_HEIGHT = 16;

static int8_t s_selectedIcon = -1; // -1 is none, 0-6 says what icon
static bool s_showingAttentionIcon = false;
static bool s_js_ready;
static bool s_pixelsChanged = false;

static bool_t s_screen_buffer[LCD_HEIGHT][LCD_WIDTH] = {{0}};
static u12_t g_program[6144] = {0};
static bool s_hasReceivedRom = false;
static bool s_hasReceivedSaveFile = false;
static bool s_loadedFromPersist = false;  // true if we already loaded state from local watch storage
static bool s_clearTextLayerOnScreenRefresh = false;
static flat_state_t stateToLoad = {0};

//ticks
static AppTimer *milli_tick_handler;
static AppTimer *screen_tick_handler;

// Local boot from app resource + persist storage.
// This default can be overridden at runtime via settings.
#define RESOURCE_BOOT_ENABLED 1

// Persistent settings keys (separate from save-state keys)
#define PERSIST_KEY_USE_EMBEDDED_ROM  200
#define PERSIST_KEY_VIBRATION_ENABLED 201
#define PERSIST_KEY_SOUND_ENABLED     202
#define PERSIST_KEY_SOUND_VOLUME      203

// Runtime settings — loaded from persist on startup, updated from Clay config.
static bool s_use_embedded_rom  = true;
static bool s_vibration_enabled = true;
static bool s_sound_enabled     = false;  // OFF by default — opt-in feature
static uint8_t s_sound_volume   = 60;

// Tama buzzer state — Stefan Bauwens' approach from the dev branch.
// Just store the frequency on set_frequency, and call speaker_play_tone()
// directly from play_frequency. Stefan confirmed this is what he ships.
static uint16_t s_speakerFreq = 0;

// Auto-save: every N minutes, persist state to local watch storage.
// Uses persist_write_data() — no AppMessage, no phone roundtrip, no xhr.
// Much more robust than the AppMessage-based save+quit flow.
#define AUTOSAVE_ENABLED 1
#define AUTOSAVE_INTERVAL_MS (5 * 60 * 1000)
static AppTimer *s_autosave_timer = NULL;

// Persist storage layout — we split flat_state_t across 3 keys because
// each persist key is limited to 256 bytes. We also write a magic number
// + version so we can detect corrupt/old data.
#define PERSIST_KEY_MAGIC         100
#define PERSIST_KEY_STATE_HEADER  101  // registers, timers, interrupts
#define PERSIST_KEY_STATE_MEM1    102  // first half of memory[]
#define PERSIST_KEY_STATE_MEM2    103  // second half of memory[]
#define PERSIST_KEY_ICONS         104  // selected_icon + showing_attention_icon

#define PERSIST_MAGIC_VALUE       0x54414D41  // 'TAMA' in ASCII
#define PERSIST_VERSION           1

static void Quit()
{
  window_stack_pop_all(false);
}

static void Message(const char * text) // Write message to screen
{
    layer_set_hidden((Layer *)s_screen_layer, true); // hide screen layer so we can read text
    text_layer_set_text(s_text_layer, text);
}

/*****************************/
/*   START HAL T FUNCTIONS   */
/*****************************/
static void * hal_malloc(u32_t size) { return 0; } // unused
static void hal_free(void *ptr) { } // unused

static void hal_halt(void)
{
	//Not yet implemented
}

static bool_t hal_is_log_enabled(log_level_t level) { return false; } // unused
static void hal_log(log_level_t level, char *buff, ...) { } // unused

static timestamp_t hal_get_timestamp(void)
{
  time_t seconds;
  uint16_t milliseconds;
  time_ms(&seconds, &milliseconds);

  //return microseconds
  return (int)seconds * 1000000 + (int)milliseconds * 1000; 
}

static void hal_sleep_until(timestamp_t ts) //this makes the time be accurate
{
  while((int) (ts - hal_get_timestamp()) > 0);
}

static void hal_update_screen(void) //since we're not using tamalib_mainloop we must call this ourselves
{
  layer_mark_dirty(s_screen_layer); //Tell the system to redraw screen
}

static void hal_set_lcd_matrix(u8_t x, u8_t y, bool_t val)
{
  s_screen_buffer[y][x] = val;
  s_pixelsChanged = true;
}

static void hal_set_lcd_icon(u8_t icon, bool_t val)
{
  if (icon == 7)
  {
    s_showingAttentionIcon = val;
  }
  else
  {
    if (!val && s_selectedIcon == icon)
    {
      s_selectedIcon = -1;
    }
    else if (val)
    {
      s_selectedIcon = icon;
    }
  }
  layer_mark_dirty(s_icons_layer);
}

// Vibration on Tama buzzer activity. Goal: exactly one vibration when
// the tama starts asking for attention. The cooldown is anchored to the
// rising edge of the attention icon — not to the buzzer or last vibration.
// So while the user is dismissing the request through menus (icon still on,
// buzzer toggling), we don't keep vibrating.
//
// Lifecycle:
//   attention icon off->on: arm vibration, ready to fire on next buzzer-on
//   buzzer off->on while armed: vibrate, disarm
//   attention icon on->off: reset (next icon-on will arm again)
static bool s_buzzer_on = false;          // current buzzer state
static bool s_prev_attention = false;     // previous attention-icon state
static bool s_vibe_armed = false;         // ready to vibrate on next buzzer

static void hal_set_frequency(u32_t freq)
{
  s_speakerFreq = freq;
}

static void hal_play_frequency(bool_t en)
{
  // --- Sound: exact same approach as Stefan Bauwens' dev branch
  if (en)
  {
#if defined(PBL_SPEAKER)
    if (s_sound_enabled) {
      speaker_stop();
      speaker_play_tone(s_speakerFreq / 10, 5000, 100, SpeakerWaveformSquare);
    }
#endif
  }
  else
  {
#if defined(PBL_SPEAKER)
    if (s_sound_enabled) {
      speaker_stop();
    }
#endif
  }

  // --- Vibration: keep our attention-icon-anchored logic
  if (s_showingAttentionIcon && !s_prev_attention) {
    s_vibe_armed = true;
  }
  if (!s_showingAttentionIcon && s_prev_attention) {
    s_vibe_armed = false;
  }
  s_prev_attention = s_showingAttentionIcon;

  if (en && !s_buzzer_on && s_vibe_armed && s_vibration_enabled) {
    vibes_long_pulse();
    s_vibe_armed = false;
  }

  s_buzzer_on = en;
}

static int hal_handler(void)
{
  return 0;
  //Not implemented as we're not using the tamalib_mainloop()
}

static hal_t hal = {
	.malloc = &hal_malloc,
	.free = &hal_free,
	.halt = &hal_halt,
	.is_log_enabled = &hal_is_log_enabled,
	.log = &hal_log,
	.sleep_until = &hal_sleep_until,
	.get_timestamp = &hal_get_timestamp,
	.update_screen = &hal_update_screen,
	.set_lcd_matrix = &hal_set_lcd_matrix,
	.set_lcd_icon = &hal_set_lcd_icon,
	.set_frequency = &hal_set_frequency,
	.play_frequency = &hal_play_frequency,
	.handler = &hal_handler,
};

/***************************/
/*   END HAL T FUNCTIONS   */
/***************************/

void set_screen_to_last_state(uint8_t *fullRam) { // gets screen data from memory and sets it to the screen
    uint8_t vram[VRAM_SIZE];

    memcpy(vram, fullRam + 320, VRAM_SIZE);

    uint8_t adjustedVram[VRAM_SIZE];
    int idx = 0;

    // Helper macro to copy forward
    #define COPY_RANGE(start, end) \
        for (int i = (start); i < (end); i++) { \
            adjustedVram[idx++] = vram[i]; \
        }

    // Helper macro to copy reversed
    #define COPY_RANGE_REVERSE(start, end) \
        for (int i = (end) - 1; i >= (start); i--) { \
            adjustedVram[idx++] = vram[i]; \
        }

    // Replicating your JS slices
    COPY_RANGE(0, 8);
    COPY_RANGE(9, 17);
    COPY_RANGE_REVERSE(29, 37);
    COPY_RANGE_REVERSE(20, 28);
    COPY_RANGE(40, 48);
    COPY_RANGE(49, 57);
    COPY_RANGE_REVERSE(69, 77);
    COPY_RANGE_REVERSE(60, 68);

    int totalBytes = idx;

    for (int i = 0; i < totalBytes; i++) {
        uint8_t byte = adjustedVram[i];

        int x = (i % BYTES_PER_LINE);
        int baseY = (i / BYTES_PER_LINE) * BITS_PER_BYTE;

        for (int bitIndex = 0; bitIndex < BITS_PER_BYTE; bitIndex++) {
          int bit = (byte >> bitIndex) & 1;  
          //int bit = (byte >> (7 - bitIndex)) & 1;

            int y = baseY + bitIndex;

            hal.set_lcd_matrix(x, y, bit);
        }
    }

    #undef COPY_RANGE
    #undef COPY_RANGE_REVERSE
}

static void milli_tick() //runs once every ms.
{
  if (s_hasReceivedRom && s_hasReceivedSaveFile)
  {
    for (size_t i = 0; i < STEPS_PER_DELAY; i++)
    {
        tamalib_step();
    } 
  } 
  milli_tick_handler = app_timer_register(STEP_DELAY, milli_tick, NULL); // calls itself in 1ms
}

static void screen_tick() // runs every 33 ms for about 30fps
{
  if (s_hasReceivedRom && s_hasReceivedSaveFile)
  {
    hal_update_screen();
  }

  if (s_clearTextLayerOnScreenRefresh)
  {
    s_clearTextLayerOnScreenRefresh = false;
    Message(" ");
    layer_set_hidden((Layer *)s_screen_layer, false); // unhide screen layer
  }

  screen_tick_handler = app_timer_register(FPS_DELAY, screen_tick, NULL);
}

// Button presses
static void on_button_up_press(ClickRecognizerRef recognizer, void *context) //1
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_LEFT, BTN_STATE_PRESSED);
}
static void on_button_select_press(ClickRecognizerRef recognizer, void *context) //2
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_MIDDLE, BTN_STATE_PRESSED);
}
static void on_button_down_press(ClickRecognizerRef recognizer, void *context) //3
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_RIGHT, BTN_STATE_PRESSED);
}


// Button releases
static void on_button_up_release(ClickRecognizerRef recognizer, void *context) //4
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_LEFT, BTN_STATE_RELEASED);
}
static void on_button_select_release(ClickRecognizerRef recognizer, void *context) //5
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_MIDDLE, BTN_STATE_RELEASED);
}
static void on_button_down_release(ClickRecognizerRef recognizer, void *context) //6
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return;
  tamalib_set_button(BTN_RIGHT, BTN_STATE_RELEASED);
}

static void on_button_back(ClickRecognizerRef recognizer, void *context) //back
{
  // Always persist locally first (fast, synchronous, guaranteed).
  // Then trigger the JS save+quit flow (which goes through phone roundtrip).
  persistSaveState();
  saveCurrentStateAndQuit(); 
}

static void click_config_provider(void *context) {
  // subscribe to button presses here
  window_raw_click_subscribe(BUTTON_ID_UP, on_button_up_press, on_button_up_release, NULL);
  window_raw_click_subscribe(BUTTON_ID_SELECT, on_button_select_press, on_button_select_release, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN, on_button_down_press, on_button_down_release, NULL);
  
  window_single_click_subscribe(BUTTON_ID_BACK, on_button_back);
}

// Handles drawing icons layers
static void icons_update_proc(Layer *layer, GContext *ctx) {
  // Set the draw color
  graphics_context_set_fill_color(ctx, GColorBlack);

  // Set the compositing mode (GCompOpSet is required for transparency)
  graphics_context_set_compositing_mode(ctx, GCompOpSet);

  if(s_selectedIcon >= 0)
  {
    #if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
    uint8_t xPos = 12 + ((s_selectedIcon%4) * 40); 
    uint8_t yPos = (s_selectedIcon > 3 ? 120 : 0);
    #else
    uint8_t xPos = 12 + ((s_selectedIcon%4) * 32);
    uint8_t yPos = (s_selectedIcon > 3 ? 100 : 0);
    #endif

    GBitmap* selected_icon = s_bitmap_icon7;
    switch(s_selectedIcon)
    {
      case 0:
        selected_icon = s_bitmap_icon1;
        break;
      case 1:
        selected_icon = s_bitmap_icon2;
        break;
      case 2:
        selected_icon = s_bitmap_icon3;
        break;
      case 3:
        selected_icon = s_bitmap_icon4;
        break;
      case 4:
        selected_icon = s_bitmap_icon5;
        break;
      case 5:
        selected_icon = s_bitmap_icon6;
        break;
      case 6:
        selected_icon = s_bitmap_icon7;
        break;
      default:
        selected_icon = s_bitmap_icon7;
        break;
    }

    // Draw selected icon if selected
    #if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
    graphics_draw_bitmap_in_rect(ctx, selected_icon, GRect(xPos, yPos, 27, 22));
    #else
    graphics_draw_bitmap_in_rect(ctx, selected_icon, GRect(xPos, yPos, 22, 18));
    #endif
  }

  // Handle attention icon
  if(s_showingAttentionIcon)
  {
    #if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
    graphics_draw_bitmap_in_rect(ctx, s_bitmap_icon8, GRect(12+(40*3), 120, 27, 22)); 
    #else
    graphics_draw_bitmap_in_rect(ctx, s_bitmap_icon8, GRect(108, 100, 22, 18));
    #endif
  }
}

// Handles drawing screen layer
static void screen_update_proc(Layer *layer, GContext *ctx) { 
  // draw new screen
  graphics_context_set_fill_color(ctx, GColorBlack);

  //draw pixels
  for (size_t h = 0; h < LCD_HEIGHT; h++)
  {
    for (size_t w = 0; w < LCD_WIDTH; w++)
    {
      if (s_screen_buffer[h][w])
      {
        #if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
        graphics_fill_rect(ctx, GRect(w * 5, h * 5, 4, 4), 0, GCornerNone);
        #else
        graphics_fill_rect(ctx, GRect(w * 4, h * 4, 3, 3), 0, GCornerNone);
        #endif
      }
    }
  }
}

static void prv_inbox_received_handler(DictionaryIterator *iter, void *context) {  
  APP_LOG(APP_LOG_LEVEL_DEBUG, "inbox received");

  Tuple *ready_tuple_t = dict_find(iter, MESSAGE_KEY_JSReady);

  if(ready_tuple_t && !s_js_ready) {
    // PebbleKit JS is ready! Safe to send messages
    s_js_ready = true;
    // Only show the "loading from phone" message if we don't have a ROM yet.
    // If we already booted from local resource, this would just overlay text
    // on top of a happily-running emulator.
    if (!s_hasReceivedRom) {
      Message("Loading ROM 0%");
    } else {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "JS ready but we already booted locally");
    }
  }

  Tuple *reset_tamagotchi_t = dict_find(iter, MESSAGE_KEY_reset_tamagotchi);
  if (reset_tamagotchi_t)
  {
    if (s_hasReceivedRom)
    {
      // Clear local persist storage too so we don't restore old state on next start
      persist_delete(PERSIST_KEY_MAGIC);
      persist_delete(PERSIST_KEY_STATE_HEADER);
      persist_delete(PERSIST_KEY_STATE_MEM1);
      persist_delete(PERSIST_KEY_STATE_MEM2);
      persist_delete(PERSIST_KEY_ICONS);
      s_loadedFromPersist = false;

      s_clearTextLayerOnScreenRefresh = true;
      s_hasReceivedSaveFile = false;
      initTamalib();
    }
  }

  // Settings updates from Clay
  Tuple *use_embedded_rom_t = dict_find(iter, MESSAGE_KEY_UseEmbeddedRom);
  if (use_embedded_rom_t) {
    bool v = (use_embedded_rom_t->value->int32 != 0);
    s_use_embedded_rom = v;
    persist_write_bool(PERSIST_KEY_USE_EMBEDDED_ROM, v);
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: UseEmbeddedRom = %d (takes effect next start)", (int)v);
  }
  Tuple *vibration_enabled_t = dict_find(iter, MESSAGE_KEY_VibrationEnabled);
  if (vibration_enabled_t) {
    bool v = (vibration_enabled_t->value->int32 != 0);
    s_vibration_enabled = v;
    persist_write_bool(PERSIST_KEY_VIBRATION_ENABLED, v);
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: VibrationEnabled = %d", (int)v);
  }
  Tuple *sound_enabled_t = dict_find(iter, MESSAGE_KEY_SoundEnabled);
  if (sound_enabled_t) {
    bool v = (sound_enabled_t->value->int32 != 0);
    s_sound_enabled = v;
    persist_write_bool(PERSIST_KEY_SOUND_ENABLED, v);
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: SoundEnabled = %d", (int)v);
  }
  Tuple *sound_volume_t = dict_find(iter, MESSAGE_KEY_SoundVolume);
  if (sound_volume_t) {
    int v = sound_volume_t->value->int32;
    if (v < 0) v = 0; else if (v > 100) v = 100;
    s_sound_volume = (uint8_t)v;
    persist_write_int(PERSIST_KEY_SOUND_VOLUME, v);
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: SoundVolume = %d", v);
  }
  

  // handle incoming rom
  Tuple *offset_t = dict_find(iter, MESSAGE_KEY_ROMOffset);
  Tuple *chunk_t = dict_find(iter, MESSAGE_KEY_ROMChunk);

  if(offset_t && chunk_t) {
    // If we already loaded the ROM locally from resource, ignore phone-sent chunks.
    // Otherwise tamalib is already running on g_program[] and we'd corrupt it.
    if (s_hasReceivedRom) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Ignoring ROM chunk (already have local ROM)");
      return;
    }
    APP_LOG(APP_LOG_LEVEL_DEBUG, "chunk received");
    int offset = offset_t->value->int16;
    uint8_t *chunk = chunk_t->value->data;
    int index = 0;

    // Convert bytes → u12_t values
    for (int i = 0; i < chunk_t->length; i += 2) {
        index = (offset + i) / 2;

        if (index >= 6144) break; // safety

        u12_t value = chunk[i] | (chunk[i + 1] << 8);

        g_program[index] = value & 0x0FFF; // ensure 12-bit

        static char progress_text[25];
        int percentage = (offset * 100)/12288;
        snprintf(progress_text, sizeof(progress_text), "Loading ROM %d%%", percentage);
        Message(progress_text);
    }
    if (index == 6143)
    {
      // we reached the end and can safely start now
      Message("Loading ROM 100%");
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Reached end of ROM!");
      s_hasReceivedRom = true;

      // Check local watch storage first — if we have a recent persist-state,
      // use it and skip waiting for JS to send one. JS will still send its
      // copy but we'll just ignore it (saveStateDict won't replace what
      // we already loaded; cpu_init_from_state was already called).
      if (persistLoadState())
      {
        APP_LOG(APP_LOG_LEVEL_INFO, "Using local persist state, skipping JS save");
        s_hasReceivedSaveFile = true;
        s_loadedFromPersist = true;
        s_clearTextLayerOnScreenRefresh = true;  // clear "Loading ROM 100%"
        initTamalib();
      }
      // else: wait for save file from js (original behavior)
    }
  }

  // Handle (error) messages
  Tuple *JSMessage_t = dict_find(iter, MESSAGE_KEY_JSMessage);
  if (JSMessage_t)
  {
    char *jsMessage = JSMessage_t->value->cstring;
    Message(jsMessage);
  }

  Tuple *JSFinishedSaving_t = dict_find(iter, MESSAGE_KEY_JSFinishedSaving);
  if (JSFinishedSaving_t)
  {
    Quit(); // we quit gracefully
  }

  // Handle incoming save state
  Tuple *STATEnone_t = dict_find(iter, MESSAGE_KEY_STATEnone);

  Tuple *STATEpc_t = dict_find(iter, MESSAGE_KEY_STATEpc);
  Tuple *STATEx_t = dict_find(iter, MESSAGE_KEY_STATEx);
  Tuple *STATEy_t = dict_find(iter, MESSAGE_KEY_STATEy);
  Tuple *STATEa_t = dict_find(iter, MESSAGE_KEY_STATEa);
  Tuple *STATEb_t = dict_find(iter, MESSAGE_KEY_STATEb);
  Tuple *STATEnp_t = dict_find(iter, MESSAGE_KEY_STATEnp);
  Tuple *STATEsp_t = dict_find(iter, MESSAGE_KEY_STATEsp);
  Tuple *STATEflags_t = dict_find(iter, MESSAGE_KEY_STATEflags);

  Tuple *STATEtick_counter_t = dict_find(iter, MESSAGE_KEY_STATEtick_counter);
  Tuple *STATEclk_timer_timestamp_t = dict_find(iter, MESSAGE_KEY_STATEclk_timer_timestamp);
  Tuple *STATEprog_timer_timestamp_t = dict_find(iter, MESSAGE_KEY_STATEprog_timer_timestamp);
  Tuple *STATEprog_timer_enabled_t = dict_find(iter, MESSAGE_KEY_STATEprog_timer_enabled);
  Tuple *STATEprog_timer_data_t = dict_find(iter, MESSAGE_KEY_STATEprog_timer_data);
  Tuple *STATEprog_timer_rld_t = dict_find(iter, MESSAGE_KEY_STATEprog_timer_rld);
  Tuple *STATEcall_depth_t = dict_find(iter, MESSAGE_KEY_STATEcall_depth);

  Tuple *STATEinterrupts_t = dict_find(iter, MESSAGE_KEY_STATEinterrupts);
  Tuple *STATEmemory_t = dict_find(iter, MESSAGE_KEY_STATEmemory);

  Tuple *STATEselected_icon_t = dict_find(iter, MESSAGE_KEY_STATEselected_icon);
  Tuple *STATEshowing_attention_icon_t = dict_find(iter, MESSAGE_KEY_STATEshowing_attention_icon);

  if(STATEnone_t)
  {
    // If we already booted from local resources, ignore the "no save" signal
    // from phone — we already started up just fine.
    if (s_hasReceivedSaveFile) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Ignoring STATEnone (already running)");
    } else {
      s_clearTextLayerOnScreenRefresh = true;
      initTamalib();
    }
  }

  if (STATEpc_t && STATEmemory_t) // assume whole block
  {
    if (s_loadedFromPersist || s_hasReceivedSaveFile) {
      // We already booted (from persist or just running fresh from resource).
      // Ignore the JS-side state to avoid overwriting good data.
      APP_LOG(APP_LOG_LEVEL_INFO, "Ignoring JS state (already running)");
      return;
    }

    Message("Loading save state...");

    uint16_t state_pc = STATEpc_t->value->uint16;
    uint16_t state_x = STATEx_t->value->uint16;
    uint16_t state_y = STATEy_t->value->uint16;
    uint8_t state_a = STATEa_t->value->uint8;
    uint8_t state_b = STATEb_t->value->uint8;
    uint8_t state_np = STATEnp_t->value->uint8;
    uint8_t state_sp = STATEsp_t->value->uint8;
    uint8_t state_flags = STATEflags_t->value->uint8;

    uint32_t state_tick_counter = STATEtick_counter_t->value->uint32;
    uint32_t state_clk_timer_timestamp = STATEclk_timer_timestamp_t->value->uint32;
    uint32_t state_prog_timer_timestamp = STATEprog_timer_timestamp_t->value->uint32;
    uint8_t state_prog_timer_enabled = STATEprog_timer_enabled_t->value->uint8;
    uint8_t state_prog_timer_data = STATEprog_timer_data_t->value->uint8;
    uint8_t state_prog_timer_rld = STATEprog_timer_rld_t->value->uint8;
    uint32_t state_call_depth = STATEcall_depth_t->value->uint32;

    uint8_t *state_interrupts = STATEinterrupts_t->value->data;
    uint8_t *state_memory = STATEmemory_t->value->data;

    stateToLoad.pc = state_pc;
    stateToLoad.x = state_x;
    stateToLoad.y = state_y;
    stateToLoad.a = state_a;
    stateToLoad.b = state_b;
    stateToLoad.np = state_np;
    stateToLoad.sp = state_sp;
    stateToLoad.flags = state_flags;

    stateToLoad.tick_counter = state_tick_counter;
    stateToLoad.clk_timer_timestamp = state_clk_timer_timestamp;
    stateToLoad.prog_timer_timestamp = state_prog_timer_timestamp;
    stateToLoad.prog_timer_enabled = state_prog_timer_enabled;
    stateToLoad.prog_timer_data = state_prog_timer_data;
    stateToLoad.prog_timer_rld = state_prog_timer_rld;
    stateToLoad.call_depth = state_call_depth;

    stateToLoad.interrupts[0].factor_flag_reg = state_interrupts[0];
    stateToLoad.interrupts[0].mask_reg        = state_interrupts[1];  
    stateToLoad.interrupts[0].triggered       = state_interrupts[2];  
    stateToLoad.interrupts[0].vector          = state_interrupts[3];  
    stateToLoad.interrupts[1].factor_flag_reg = state_interrupts[4];
    stateToLoad.interrupts[1].mask_reg        = state_interrupts[5];  
    stateToLoad.interrupts[1].triggered       = state_interrupts[6];  
    stateToLoad.interrupts[1].vector          = state_interrupts[7];  
    stateToLoad.interrupts[2].factor_flag_reg = state_interrupts[8];
    stateToLoad.interrupts[2].mask_reg        = state_interrupts[9];  
    stateToLoad.interrupts[2].triggered       = state_interrupts[10];  
    stateToLoad.interrupts[2].vector          = state_interrupts[11]; 
    stateToLoad.interrupts[3].factor_flag_reg = state_interrupts[12];
    stateToLoad.interrupts[3].mask_reg        = state_interrupts[13];  
    stateToLoad.interrupts[3].triggered       = state_interrupts[14];  
    stateToLoad.interrupts[3].vector          = state_interrupts[15]; 
    stateToLoad.interrupts[4].factor_flag_reg = state_interrupts[16];
    stateToLoad.interrupts[4].mask_reg        = state_interrupts[17];  
    stateToLoad.interrupts[4].triggered       = state_interrupts[18];  
    stateToLoad.interrupts[4].vector          = state_interrupts[19];         
    stateToLoad.interrupts[5].factor_flag_reg = state_interrupts[20];
    stateToLoad.interrupts[5].mask_reg        = state_interrupts[21];  
    stateToLoad.interrupts[5].triggered       = state_interrupts[22];  
    stateToLoad.interrupts[5].vector          = state_interrupts[23]; 

    memcpy(stateToLoad.memory, state_memory, sizeof(stateToLoad.memory));

    s_hasReceivedSaveFile = true;
    s_clearTextLayerOnScreenRefresh = true;

    if (STATEselected_icon_t && STATEshowing_attention_icon_t)
    {
      s_selectedIcon = STATEselected_icon_t->value->int8;
      s_showingAttentionIcon = STATEshowing_attention_icon_t->value->int8;

      layer_mark_dirty(s_icons_layer);
    }

    initTamalib();
  }
}

// Time + date indicators in the watch frame area (above and below the tama LCD).
// Updated via AppTimer every 30s — safe and doesn't reintroduce the
// tick_timer_service issue we had earlier.
static AppTimer *s_clock_timer = NULL;

static void update_clock_text(void)
{
  if (!s_time_layer || !s_date_layer) return;
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t) return;

  // Time: HH:MM (24h)
  strftime(s_time_text, sizeof(s_time_text), "%H:%M", t);
  text_layer_set_text(s_time_layer, s_time_text);

  // Date: e.g. "Mo 21.05"
  strftime(s_date_text, sizeof(s_date_text), "%a %d.%m", t);
  text_layer_set_text(s_date_layer, s_date_text);
}

static void clock_timer_callback(void *data)
{
  s_clock_timer = NULL;
  update_clock_text();
  s_clock_timer = app_timer_register(30 * 1000, clock_timer_callback, NULL);
}

// Battery indicator: small text at bottom of screen showing percent + charging.
static void battery_handler(BatteryChargeState state)
{
  if (!s_battery_layer) return;
  snprintf(s_battery_text, sizeof(s_battery_text), "%s%d%%",
           state.is_charging ? "+" : "",
           (int)state.charge_percent);
  text_layer_set_text(s_battery_layer, s_battery_text);
}

static void main_window_load(Window *window) {
  // Get information about the Window
  Layer *window_layer = window_get_root_layer(window);

  // Create GBitmap for background 
#if defined(PBL_COLOR)
  s_bitmap_bg = gbitmap_create_with_resource(RESOURCE_ID_BG_IMAGE);
#else
  s_bitmap_bg = gbitmap_create_with_resource(RESOURCE_ID_BG_IMAGE_BW);
#endif

  // Create background layer
#if defined(PBL_PLATFORM_CHALK)
  s_background_layer = bitmap_layer_create(GRect(0, 0, 180, 180));
#elif defined(PBL_PLATFORM_GABBRO)
    s_background_layer = bitmap_layer_create(GRect(0, 0, 260, 260));
#elif defined(PBL_PLATFORM_EMERY)
    s_background_layer = bitmap_layer_create(GRect(0, 0, 200, 228));
#else
  s_background_layer = bitmap_layer_create(GRect(0, 0, 144, 168));
#endif
  bitmap_layer_set_compositing_mode(s_background_layer, GCompOpSet);
  bitmap_layer_set_bitmap(s_background_layer, s_bitmap_bg);

  // Add it as a child layer to the Window's root layer
  layer_add_child(window_layer, bitmap_layer_get_layer(s_background_layer));

  // Create bitmaps for icons
  s_bitmap_icon1 = gbitmap_create_with_resource(RESOURCE_ID_ICON1);
  s_bitmap_icon2 = gbitmap_create_with_resource(RESOURCE_ID_ICON2);
  s_bitmap_icon3 = gbitmap_create_with_resource(RESOURCE_ID_ICON3);
  s_bitmap_icon4 = gbitmap_create_with_resource(RESOURCE_ID_ICON4);
  s_bitmap_icon5 = gbitmap_create_with_resource(RESOURCE_ID_ICON5);
  s_bitmap_icon6 = gbitmap_create_with_resource(RESOURCE_ID_ICON6);
  s_bitmap_icon7 = gbitmap_create_with_resource(RESOURCE_ID_ICON7);
  s_bitmap_icon8 = gbitmap_create_with_resource(RESOURCE_ID_ICON8);

  // Create icons layer
#if defined(PBL_PLATFORM_CHALK)
  s_icons_layer = layer_create(GRect(0+18, 24+6, 144, 146));
#elif defined(PBL_PLATFORM_GABBRO) 
  s_icons_layer = layer_create(GRect(0+45, 60, 180, 183)); 
#elif defined(PBL_PLATFORM_EMERY)
  s_icons_layer = layer_create(GRect(0+15, 44, 180, 183)); 
#else
  s_icons_layer = layer_create(GRect(0, 24, 144, 146));
#endif
  layer_set_update_proc(s_icons_layer, icons_update_proc);

  // Add to window
  layer_add_child(window_layer, s_icons_layer);

  // Create screen Layer
#if defined(PBL_PLATFORM_CHALK)
  s_screen_layer = layer_create(GRect(8+18, 51+6, 128, 64));
#elif defined(PBL_PLATFORM_GABBRO)
  s_screen_layer = layer_create(GRect(50, 92, 160, 80));
#elif defined(PBL_PLATFORM_EMERY)
  s_screen_layer = layer_create(GRect(20, 76, 160, 80));
#else
  s_screen_layer = layer_create(GRect(8, 51, 128, 64));
#endif
  layer_set_update_proc(s_screen_layer, screen_update_proc);

  // Add to window  
  layer_add_child(window_layer, s_screen_layer);

  // Font
  //s_lcd_font    = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SMALL_LCD_9));

  // Create text layer
  #if defined(PBL_PLATFORM_CHALK)
  s_text_layer = text_layer_create(GRect(6+18, 60+6, 128, 50)); 
  #elif defined(PBL_PLATFORM_GABBRO)
  s_text_layer = text_layer_create(GRect(50, 60+46, 158, 50));
  #elif defined(PBL_PLATFORM_EMERY)
  s_text_layer = text_layer_create(GRect(20, 60+30, 158, 50));
  #else   
  s_text_layer = text_layer_create(GRect(6, 60, 128, 50)); 
  #endif
  text_layer_set_background_color(s_text_layer, GColorClear);
  //text_layer_set_font(s_text_layer, s_lcd_font);
  Message("Waiting for phone...");
  text_layer_set_text_alignment(s_text_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_text_layer, GTextOverflowModeWordWrap);
  layer_add_child(window_layer, text_layer_get_layer(s_text_layer));

  // Battery (right) + Time (left) at top, Date at bottom — all on the watch frame.
  // Sizes & positions tuned per platform; weiße fette Schrift gegen dunklen Rahmen.

#if defined(PBL_PLATFORM_GABBRO)
  // Pebble Time 2: 260x260, tama LCD at y=92-172
  s_time_layer    = text_layer_create(GRect(10,   2, 130, 50));
  s_battery_layer = text_layer_create(GRect(140, 10, 115, 36));
  s_date_layer    = text_layer_create(GRect(10, 215, 240, 32));
  GFont time_font    = fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD);
  GFont small_font   = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
#elif defined(PBL_PLATFORM_EMERY)
  // Emery: 200x228, tama LCD at y=76-156
  s_time_layer    = text_layer_create(GRect(8,   2, 110, 42));
  s_battery_layer = text_layer_create(GRect(118, 8, 80, 30));
  s_date_layer    = text_layer_create(GRect(10, 195, 180, 26));
  GFont time_font    = fonts_get_system_font(FONT_KEY_BITHAM_34_MEDIUM_NUMBERS);
  GFont small_font   = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
#elif defined(PBL_PLATFORM_CHALK)
  // Round 180x180 — corners less useful, use top-center / bottom-center
  s_time_layer    = text_layer_create(GRect(10,   4, 80, 30));
  s_battery_layer = text_layer_create(GRect(90,   8, 80, 22));
  s_date_layer    = text_layer_create(GRect(0,  148, 180, 22));
  GFont time_font    = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont small_font   = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
#else
  // Diorite/Basalt 144x168, tama LCD at y=51-115
  s_time_layer    = text_layer_create(GRect(4,   0, 80, 28));
  s_battery_layer = text_layer_create(GRect(84,  6, 60, 22));
  s_date_layer    = text_layer_create(GRect(0, 135, 144, 22));
  GFont time_font    = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont small_font   = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
#endif

  // Common style for all three. Time gets its own (bigger) font.
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorWhite);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentLeft);
  text_layer_set_font(s_time_layer, time_font);
  layer_add_child(window_layer, text_layer_get_layer(s_time_layer));

  TextLayer *small_infos[2] = { s_battery_layer, s_date_layer };
  GTextAlignment small_aligns[2] = { GTextAlignmentRight, GTextAlignmentCenter };
  for (int i = 0; i < 2; i++) {
    text_layer_set_background_color(small_infos[i], GColorClear);
    text_layer_set_text_color(small_infos[i], GColorWhite);
    text_layer_set_text_alignment(small_infos[i], small_aligns[i]);
    text_layer_set_font(small_infos[i], small_font);
    layer_add_child(window_layer, text_layer_get_layer(small_infos[i]));
  }

  // Initial values + subscriptions
  battery_state_service_subscribe(battery_handler);
  battery_handler(battery_state_service_peek());
  update_clock_text();
  s_clock_timer = app_timer_register(30 * 1000, clock_timer_callback, NULL);

  // Sub to ticks
  milli_tick_handler = app_timer_register(STEP_DELAY, milli_tick, NULL);
  screen_tick_handler = app_timer_register(FPS_DELAY, screen_tick, NULL);
}

static void main_window_unload(Window *window) {
  // Cancel clock update timer
  if (s_clock_timer) {
    app_timer_cancel(s_clock_timer);
    s_clock_timer = NULL;
  }

  // Unsubscribe battery service
  battery_state_service_unsubscribe();
  if (s_battery_layer) {
    text_layer_destroy(s_battery_layer);
    s_battery_layer = NULL;
  }
  if (s_time_layer) {
    text_layer_destroy(s_time_layer);
    s_time_layer = NULL;
  }
  if (s_date_layer) {
    text_layer_destroy(s_date_layer);
    s_date_layer = NULL;
  }

  // Destroy backrgound bitmap and its layer
  gbitmap_destroy(s_bitmap_bg);
  bitmap_layer_destroy(s_background_layer);

  // Destroy text layer
  text_layer_destroy(s_text_layer);

  // Unload font
  //fonts_unload_custom_font(s_lcd_font);

  // Destroy icon bitmaps
  gbitmap_destroy(s_bitmap_icon1);
  gbitmap_destroy(s_bitmap_icon2);
  gbitmap_destroy(s_bitmap_icon3);
  gbitmap_destroy(s_bitmap_icon4);
  gbitmap_destroy(s_bitmap_icon5);
  gbitmap_destroy(s_bitmap_icon6);
  gbitmap_destroy(s_bitmap_icon7);
  gbitmap_destroy(s_bitmap_icon8);

  // Destory icons layer
  layer_destroy(s_icons_layer);

  // Destroy screen layer
  layer_destroy(s_screen_layer);

  // Unsubscribe to ticks
  app_timer_cancel(milli_tick_handler);
  app_timer_cancel(screen_tick_handler);
}

static void initTamalib() {
  // Register HAL
  tamalib_register_hal(&hal);

  // Check for save file
  if (s_hasReceivedSaveFile) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Save state found. Loading...");

    cpu_init_from_state(g_program, &stateToLoad, NULL, 1000000);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "cpu_init_from_state done");
    set_screen_to_last_state(stateToLoad.memory); 
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Save state loaded!");
  }
  else
  {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "No save file");
    s_hasReceivedSaveFile = true;
    tamalib_init(g_program, NULL, 1000000);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "tamalib_init done");
  }

  // Initialer RTC -> Tama Sync (in beiden Pfaden, da cpu_state nun bereit ist)
  tama_rtc_initial_sync();
}

// RTC sync via AppTimer (statt tick_timer_service). Robusterer Ansatz
// weil tick_timer_service primär für Watchfaces ist und in Watchapps
// manchmal unerwartete Side-Effects hat.
#define RTC_SYNC_CHECK_INTERVAL_MS (2 * 60 * 60 * 1000)  // alle 2 Stunden
static AppTimer *s_rtc_sync_timer = NULL;

static void rtc_sync_timer_callback(void *data)
{
  s_rtc_sync_timer = NULL;

  if (s_hasReceivedRom && s_hasReceivedSaveFile) {
    tama_rtc_periodic_check();
  }

  // Reschedule
  s_rtc_sync_timer = app_timer_register(RTC_SYNC_CHECK_INTERVAL_MS, rtc_sync_timer_callback, NULL);
}

// One-shot timer callback: try local boot after init() returned and the
// Pebble event loop is running. This avoids any lifecycle weirdness from
// initializing tamalib/timers while still inside init().
static void init() {
  // Log why we (re)started — helps debug unexpected app restarts.
  // APP_LAUNCH_TIMEOUT_TIMER_CANCELLED = OS killed us for inactivity/memory.
  APP_LOG(APP_LOG_LEVEL_INFO, "App launched. reason=%d", (int)launch_reason());

  // Create main Window element and assign to pointer
  s_main_window = window_create();

  // Set handlers to manage the elements inside the Window
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });

  // Auto RTC -> Tama sync via AppTimer (alle 2h, mit Drift-Toleranz intern)
  s_rtc_sync_timer = app_timer_register(RTC_SYNC_CHECK_INTERVAL_MS, rtc_sync_timer_callback, NULL);

  // Start auto-save timer (saves every AUTOSAVE_INTERVAL_MS without quitting).
  // Disabled by default — flip AUTOSAVE_ENABLED above to 1 to enable.
#if AUTOSAVE_ENABLED
  s_autosave_timer = app_timer_register(AUTOSAVE_INTERVAL_MS, autosave_timer_callback, NULL);
  APP_LOG(APP_LOG_LEVEL_INFO, "Auto-save enabled, interval=%d ms", AUTOSAVE_INTERVAL_MS);
#else
  APP_LOG(APP_LOG_LEVEL_INFO, "Auto-save disabled");
#endif

  // Show the Window on the watch, with animated=true
  window_stack_push(s_main_window, true);

  // Load user settings from persist before anything that depends on them
  loadSettingsFromPersist();

  // Try local boot if enabled by user setting (default: on)
  if (s_use_embedded_rom) {
    if (loadRomFromResource()) {
      s_hasReceivedRom = true;
      s_clearTextLayerOnScreenRefresh = true;

      if (persistLoadState()) {
        APP_LOG(APP_LOG_LEVEL_INFO, "Local boot: ROM + persist state loaded");
        s_hasReceivedSaveFile = true;
        s_loadedFromPersist = true;
        initTamalib();
      } else {
        APP_LOG(APP_LOG_LEVEL_INFO, "Local boot: ROM loaded, no persist state -> fresh start");
        initTamalib();
      }
    } else {
      APP_LOG(APP_LOG_LEVEL_WARNING, "No local ROM resource — falling back to phone");
    }
  } else {
    APP_LOG(APP_LOG_LEVEL_INFO, "Embedded ROM disabled by setting — using phone-driven boot");
  }

  // Open AppMessage connection
  app_message_register_inbox_received(prv_inbox_received_handler);
  app_message_open(2048, 2048);

  // Listen for button events
  window_set_click_config_provider(s_main_window, click_config_provider);
}

static void saveCurrentState(bool isAutoSave)
{
  if (!s_hasReceivedRom)
  {
    if (!isAutoSave) Quit(); // manual save without rom: just quit
    return; // auto-save without rom: skip silently
  }

  if (!isAutoSave) {
    Message("Saving state...");
  }
  
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Getting save file and sending to phone... (autosave=%d)", (int)isAutoSave);

  // Send save file to phone 
  flat_state_t saveState = cpu_get_flat_state();

  // Declare the dictionary's iterator
  DictionaryIterator *out_iter;

  // Prepare the outbox buffer for this message
  AppMessageResult result = app_message_outbox_begin(&out_iter);
  if(result == APP_MSG_OK) {
    // Mark this message as auto-save so JS knows not to send JSFinishedSaving back
    uint8_t autosave_flag = isAutoSave ? 1 : 0;
    dict_write_int(out_iter, MESSAGE_KEY_AutoSave, &autosave_flag, sizeof(uint8_t), false);

    // Construct the message
    dict_write_int(out_iter, MESSAGE_KEY_STATEpc, &saveState.pc, sizeof(uint16_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEx, &saveState.x, sizeof(uint16_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEy, &saveState.y, sizeof(uint16_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEa, &saveState.a, sizeof(uint8_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEb, &saveState.b, sizeof(uint8_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEnp, &saveState.np, sizeof(uint8_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEsp, &saveState.sp, sizeof(uint8_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEflags, &saveState.flags, sizeof(uint8_t), false);
    
    dict_write_int(out_iter, MESSAGE_KEY_STATEtick_counter, &saveState.tick_counter, sizeof(uint32_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEclk_timer_timestamp, &saveState.clk_timer_timestamp, sizeof(uint32_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEprog_timer_timestamp, &saveState.prog_timer_timestamp, sizeof(uint32_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEprog_timer_enabled, &saveState.prog_timer_enabled, sizeof(uint8_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEprog_timer_data, &saveState.prog_timer_data, sizeof(uint8_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEprog_timer_rld, &saveState.prog_timer_rld, sizeof(uint8_t), false);
    dict_write_int(out_iter, MESSAGE_KEY_STATEcall_depth, &saveState.call_depth, sizeof(uint32_t), false);

    uint8_t interrupts[24] = {
      saveState.interrupts[0].factor_flag_reg, saveState.interrupts[0].mask_reg, saveState.interrupts[0].triggered, saveState.interrupts[0].vector,
      saveState.interrupts[1].factor_flag_reg, saveState.interrupts[1].mask_reg, saveState.interrupts[1].triggered, saveState.interrupts[1].vector,
      saveState.interrupts[2].factor_flag_reg, saveState.interrupts[2].mask_reg, saveState.interrupts[2].triggered, saveState.interrupts[2].vector,
      saveState.interrupts[3].factor_flag_reg, saveState.interrupts[3].mask_reg, saveState.interrupts[3].triggered, saveState.interrupts[3].vector,
      saveState.interrupts[4].factor_flag_reg, saveState.interrupts[4].mask_reg, saveState.interrupts[4].triggered, saveState.interrupts[4].vector,
      saveState.interrupts[5].factor_flag_reg, saveState.interrupts[5].mask_reg, saveState.interrupts[5].triggered, saveState.interrupts[5].vector,
    };

    dict_write_data(out_iter, MESSAGE_KEY_STATEinterrupts, interrupts, sizeof(interrupts));

    dict_write_data(out_iter, MESSAGE_KEY_STATEmemory, saveState.memory, sizeof(saveState.memory));

    // handle icons
    dict_write_int(out_iter, MESSAGE_KEY_STATEselected_icon, &s_selectedIcon, sizeof(int8_t), true);
    dict_write_int(out_iter, MESSAGE_KEY_STATEshowing_attention_icon, &s_showingAttentionIcon, sizeof(int8_t), true);

     dict_write_end(out_iter);
    // Send this message
    result = app_message_outbox_send();

    // Check the result
    if(result != APP_MSG_OK) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Error sending the outbox: %d", (int)result);
      if (!isAutoSave) {
        Message("Can't send state!"); //TODO handle better
        Quit();
      }
      // auto-save failure: just log and try again next interval
    }
    else
    {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Save file sent successfully to phone! (autosave=%d)", (int)isAutoSave);
      //for non-autosave: will wait for response from js to quit
    }
  } else {
    // The outbox cannot be used right now
    APP_LOG(APP_LOG_LEVEL_ERROR, "Error preparing the outbox: %d", (int)result);
    if (!isAutoSave) {
      Message("Can't send state!"); //TODO handle better
      Quit();
    }
  }
}

static void saveCurrentStateAndQuit()
{
  saveCurrentState(false);
}

// Load ROM from app resource (built-in, no phone needed).
// Returns true on success.
static bool loadRomFromResource(void)
{
  ResHandle handle = resource_get_handle(RESOURCE_ID_TAMA_ROM);
  if (!handle) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "loadRomFromResource: no resource handle");
    return false;
  }

  size_t res_size = resource_size(handle);
  const size_t expected = 6144 * 2;  // 6144 u12 values, packed as 2 bytes each
  if (res_size != expected) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "loadRomFromResource: size %d != expected %d",
            (int)res_size, (int)expected);
    return false;
  }

  // Read into a temporary byte buffer on the heap (12 KB stays off the stack)
  uint8_t *buf = malloc(res_size);
  if (!buf) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "loadRomFromResource: malloc failed");
    return false;
  }

  size_t read = resource_load(handle, buf, res_size);
  if (read != res_size) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "loadRomFromResource: read %d != expected %d",
            (int)read, (int)res_size);
    free(buf);
    return false;
  }

  // Unpack: 2 bytes per 12-bit value, low byte first.
  // Same format as the JS-chunked-ROM-receive path.
  for (int i = 0; i < 6144; i++) {
    u12_t value = buf[i * 2] | (buf[i * 2 + 1] << 8);
    g_program[i] = value & 0x0FFF;  // ensure 12-bit
  }

  free(buf);
  APP_LOG(APP_LOG_LEVEL_INFO, "loadRomFromResource: ROM loaded (%d bytes)", (int)res_size);
  return true;
}

// Load settings from persist (with safe defaults if no values stored)
static void loadSettingsFromPersist(void)
{
  if (persist_exists(PERSIST_KEY_USE_EMBEDDED_ROM)) {
    s_use_embedded_rom = persist_read_bool(PERSIST_KEY_USE_EMBEDDED_ROM);
  }
  if (persist_exists(PERSIST_KEY_VIBRATION_ENABLED)) {
    s_vibration_enabled = persist_read_bool(PERSIST_KEY_VIBRATION_ENABLED);
  }
  if (persist_exists(PERSIST_KEY_SOUND_ENABLED)) {
    s_sound_enabled = persist_read_bool(PERSIST_KEY_SOUND_ENABLED);
  }
  if (persist_exists(PERSIST_KEY_SOUND_VOLUME)) {
    int v = persist_read_int(PERSIST_KEY_SOUND_VOLUME);
    if (v >= 0 && v <= 100) s_sound_volume = (uint8_t)v;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "Settings: embedded_rom=%d vibration=%d sound=%d vol=%d",
          (int)s_use_embedded_rom, (int)s_vibration_enabled,
          (int)s_sound_enabled, (int)s_sound_volume);
}

// Save current state to local watch storage (persist API). Synchronous,
// no AppMessage involved. Returns true on success.
static bool persistSaveState(void)
{
  if (!s_hasReceivedRom || !s_hasReceivedSaveFile) return false;

  flat_state_t st = cpu_get_flat_state();

  // Sanity check: refuse to save obviously-bad state
  if (st.pc == 0) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "persistSaveState: refusing to save state with pc=0");
    return false;
  }

  // Header: everything except the memory[] array.
  // Order/packing: explicit struct so layout is stable across builds.
  typedef struct __attribute__((packed)) {
    uint16_t pc;
    uint16_t x;
    uint16_t y;
    uint8_t  a;
    uint8_t  b;
    uint8_t  np;
    uint8_t  sp;
    uint8_t  flags;
    uint32_t tick_counter;
    uint32_t clk_timer_timestamp;
    uint32_t prog_timer_timestamp;
    uint8_t  prog_timer_enabled;
    uint8_t  prog_timer_data;
    uint8_t  prog_timer_rld;
    uint32_t call_depth;
    // 6 interrupts × 4 bytes each = 24 bytes
    uint8_t  interrupts[24];
  } persist_header_t;

  persist_header_t hdr;
  hdr.pc = st.pc;
  hdr.x = st.x;
  hdr.y = st.y;
  hdr.a = st.a;
  hdr.b = st.b;
  hdr.np = st.np;
  hdr.sp = st.sp;
  hdr.flags = st.flags;
  hdr.tick_counter = st.tick_counter;
  hdr.clk_timer_timestamp = st.clk_timer_timestamp;
  hdr.prog_timer_timestamp = st.prog_timer_timestamp;
  hdr.prog_timer_enabled = st.prog_timer_enabled;
  hdr.prog_timer_data = st.prog_timer_data;
  hdr.prog_timer_rld = st.prog_timer_rld;
  hdr.call_depth = st.call_depth;
  for (int i = 0; i < 6; i++) {
    hdr.interrupts[i*4+0] = st.interrupts[i].factor_flag_reg;
    hdr.interrupts[i*4+1] = st.interrupts[i].mask_reg;
    hdr.interrupts[i*4+2] = st.interrupts[i].triggered;
    hdr.interrupts[i*4+3] = st.interrupts[i].vector;
  }

  // Split memory[] into two halves to stay under 256-byte persist limit.
  // MEM_BUFFER_SIZE × sizeof(u4_t) bytes total — typically ~464 bytes.
  const size_t mem_total = sizeof(st.memory);
  const size_t mem_half  = mem_total / 2;
  const size_t mem_rest  = mem_total - mem_half;

  status_t s;
  s = persist_write_data(PERSIST_KEY_STATE_HEADER, &hdr, sizeof(hdr));
  if (s < 0) { APP_LOG(APP_LOG_LEVEL_ERROR, "persist header write failed: %d", (int)s); return false; }

  s = persist_write_data(PERSIST_KEY_STATE_MEM1, &st.memory[0], mem_half);
  if (s < 0) { APP_LOG(APP_LOG_LEVEL_ERROR, "persist mem1 write failed: %d", (int)s); return false; }

  s = persist_write_data(PERSIST_KEY_STATE_MEM2, ((uint8_t*)st.memory) + mem_half, mem_rest);
  if (s < 0) { APP_LOG(APP_LOG_LEVEL_ERROR, "persist mem2 write failed: %d", (int)s); return false; }

  // Icons (small, one key)
  uint8_t icons[2] = { (uint8_t)s_selectedIcon, (uint8_t)s_showingAttentionIcon };
  persist_write_data(PERSIST_KEY_ICONS, icons, sizeof(icons));

  // Magic LAST — only set after all other writes succeeded.
  // Reader checks this first; if missing/wrong, the rest is ignored.
  uint32_t magic = PERSIST_MAGIC_VALUE;
  persist_write_data(PERSIST_KEY_MAGIC, &magic, sizeof(magic));

  return true;
}

// Read persisted state into stateToLoad. Returns true if a valid state
// was found and loaded. Called during init() before tamalib_init().
static bool persistLoadState(void)
{
  APP_LOG(APP_LOG_LEVEL_DEBUG, "persistLoadState: starting");

  if (!persist_exists(PERSIST_KEY_MAGIC)) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "persistLoadState: no magic key -> first time or wiped");
    return false;
  }

  uint32_t magic = 0;
  persist_read_data(PERSIST_KEY_MAGIC, &magic, sizeof(magic));
  if (magic != PERSIST_MAGIC_VALUE) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "persistLoadState: bad magic 0x%lx", (unsigned long)magic);
    return false;
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "persistLoadState: magic ok");

  if (!persist_exists(PERSIST_KEY_STATE_HEADER) ||
      !persist_exists(PERSIST_KEY_STATE_MEM1) ||
      !persist_exists(PERSIST_KEY_STATE_MEM2)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "persistLoadState: missing keys");
    return false;
  }

  typedef struct __attribute__((packed)) {
    uint16_t pc; uint16_t x; uint16_t y;
    uint8_t  a; uint8_t b; uint8_t np; uint8_t sp; uint8_t flags;
    uint32_t tick_counter;
    uint32_t clk_timer_timestamp;
    uint32_t prog_timer_timestamp;
    uint8_t  prog_timer_enabled;
    uint8_t  prog_timer_data;
    uint8_t  prog_timer_rld;
    uint32_t call_depth;
    uint8_t  interrupts[24];
  } persist_header_t;

  persist_header_t hdr;
  int read = persist_read_data(PERSIST_KEY_STATE_HEADER, &hdr, sizeof(hdr));
  if (read != (int)sizeof(hdr)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "persistLoadState: header size mismatch %d", read);
    return false;
  }

  stateToLoad.pc = hdr.pc;
  stateToLoad.x = hdr.x;
  stateToLoad.y = hdr.y;
  stateToLoad.a = hdr.a;
  stateToLoad.b = hdr.b;
  stateToLoad.np = hdr.np;
  stateToLoad.sp = hdr.sp;
  stateToLoad.flags = hdr.flags;
  stateToLoad.tick_counter = hdr.tick_counter;
  stateToLoad.clk_timer_timestamp = hdr.clk_timer_timestamp;
  stateToLoad.prog_timer_timestamp = hdr.prog_timer_timestamp;
  stateToLoad.prog_timer_enabled = hdr.prog_timer_enabled;
  stateToLoad.prog_timer_data = hdr.prog_timer_data;
  stateToLoad.prog_timer_rld = hdr.prog_timer_rld;
  stateToLoad.call_depth = hdr.call_depth;
  for (int i = 0; i < 6; i++) {
    stateToLoad.interrupts[i].factor_flag_reg = hdr.interrupts[i*4+0];
    stateToLoad.interrupts[i].mask_reg        = hdr.interrupts[i*4+1];
    stateToLoad.interrupts[i].triggered       = hdr.interrupts[i*4+2];
    stateToLoad.interrupts[i].vector          = hdr.interrupts[i*4+3];
  }

  const size_t mem_total = sizeof(stateToLoad.memory);
  const size_t mem_half  = mem_total / 2;
  const size_t mem_rest  = mem_total - mem_half;

  int r1 = persist_read_data(PERSIST_KEY_STATE_MEM1, &stateToLoad.memory[0], mem_half);
  int r2 = persist_read_data(PERSIST_KEY_STATE_MEM2, ((uint8_t*)stateToLoad.memory) + mem_half, mem_rest);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "persistLoadState: mem reads r1=%d r2=%d expected=%d+%d",
          r1, r2, (int)mem_half, (int)mem_rest);
  if (r1 != (int)mem_half || r2 != (int)mem_rest) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "persistLoadState: memory size mismatch -> aborting");
    return false;
  }

  if (persist_exists(PERSIST_KEY_ICONS)) {
    uint8_t icons[2] = {0};
    persist_read_data(PERSIST_KEY_ICONS, icons, sizeof(icons));
    s_selectedIcon = (int8_t)icons[0];
    s_showingAttentionIcon = (bool)icons[1];
  }

  APP_LOG(APP_LOG_LEVEL_INFO, "persistLoadState: loaded state from watch storage (pc=%d)", (int)hdr.pc);
  return true;
}

// AppTimer callback: triggers persist-based auto-save and reschedules.
static void autosave_timer_callback(void *data)
{
  s_autosave_timer = NULL;

  if (s_hasReceivedRom && s_hasReceivedSaveFile) {
    if (persistSaveState()) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Auto-save: persisted to watch storage");
    }
  }

  // Reschedule
  s_autosave_timer = app_timer_register(AUTOSAVE_INTERVAL_MS, autosave_timer_callback, NULL);
}

static void deinit() {
  if (s_autosave_timer) {
    app_timer_cancel(s_autosave_timer);
    s_autosave_timer = NULL;
  }
  if (s_rtc_sync_timer) {
    app_timer_cancel(s_rtc_sync_timer);
    s_rtc_sync_timer = NULL;
  }

  window_destroy(s_main_window);

  // Release tamalib
  tamalib_release();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}