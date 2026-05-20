/*
 * tama_rtc_sync.c
 *
 * Implementation des RTC -> Tama-RAM Time-Syncs.
 * Siehe Header für Memory-Map und Begründung.
 */

#include "tama_rtc_sync.h"
#include "tamalib/cpu.h"

// Tama-RAM Adressen der laufenden Uhr (siehe tama_notes.txt)
#define TAMA_ADDR_SEC_LOW   0x010
#define TAMA_ADDR_SEC_HIGH  0x011
#define TAMA_ADDR_MIN_LOW   0x012
#define TAMA_ADDR_MIN_HIGH  0x013
#define TAMA_ADDR_HOUR_LOW  0x014  // HEX nibble, nicht BCD
#define TAMA_ADDR_HOUR_HIGH 0x015  // HEX nibble, nicht BCD

// Drift-Toleranz: erst ab > 30 s Abweichung wird re-synct.
// Verhindert ständige Mikro-Korrekturen.
#define DRIFT_TOLERANCE_SEC 30

// Auto-Sync-Intervall in Stunden. tick_timer_service ruft uns
// jede Stunde auf, wir syncen aber nur jedes 2. Mal.
#define SYNC_INTERVAL_HOURS 2

static bool s_initial_sync_done = false;

// Schreibt h/m/s direkt in den emulierten Tama-RAM.
static void write_time_to_tama(uint8_t hour, uint8_t minute, uint8_t second)
{
  state_t *s = cpu_get_state();
  if (!s || !s->memory) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "RTC sync: cpu_state nicht verfügbar");
    return;
  }

  // Sekunden + Minuten: BCD-Style (Zehner / Einer als separate Nibbles)
  SET_MEMORY(s->memory, TAMA_ADDR_SEC_LOW,   second % 10);
  SET_MEMORY(s->memory, TAMA_ADDR_SEC_HIGH,  second / 10);
  SET_MEMORY(s->memory, TAMA_ADDR_MIN_LOW,   minute % 10);
  SET_MEMORY(s->memory, TAMA_ADDR_MIN_HIGH,  minute / 10);

  // Stunden: HEX-Nibbles. Bei 21 Uhr -> low=0x5, high=0x1, rekonstruiert
  // als (high << 4) | low = 0x15 = 21 dezimal.
  SET_MEMORY(s->memory, TAMA_ADDR_HOUR_LOW,  hour & 0x0F);
  SET_MEMORY(s->memory, TAMA_ADDR_HOUR_HIGH, (hour >> 4) & 0x0F);

  // Tamalib daran hindern, die "verlorene" Zeit nachzuholen.
  cpu_sync_ref_timestamp();
}

// Liest die aktuell im Tama-RAM stehende Uhrzeit zurück.
// Gibt Sekunden seit Mitternacht zurück (uint32_t, kann nicht überlaufen).
static uint32_t read_tama_seconds_since_midnight(void)
{
  state_t *s = cpu_get_state();
  if (!s || !s->memory) return 0;

  uint8_t sec_l = GET_MEMORY(s->memory, TAMA_ADDR_SEC_LOW)   & 0x0F;
  uint8_t sec_h = GET_MEMORY(s->memory, TAMA_ADDR_SEC_HIGH)  & 0x0F;
  uint8_t min_l = GET_MEMORY(s->memory, TAMA_ADDR_MIN_LOW)   & 0x0F;
  uint8_t min_h = GET_MEMORY(s->memory, TAMA_ADDR_MIN_HIGH)  & 0x0F;
  uint8_t hr_l  = GET_MEMORY(s->memory, TAMA_ADDR_HOUR_LOW)  & 0x0F;
  uint8_t hr_h  = GET_MEMORY(s->memory, TAMA_ADDR_HOUR_HIGH) & 0x0F;

  uint8_t second = sec_h * 10 + sec_l;
  uint8_t minute = min_h * 10 + min_l;
  uint8_t hour   = (hr_h << 4) | hr_l;  // HEX-Reconstruction

  return (uint32_t)hour * 3600U + (uint32_t)minute * 60U + (uint32_t)second;
}

static uint32_t seconds_since_midnight_from_tm(const struct tm *t)
{
  return (uint32_t)t->tm_hour * 3600U +
         (uint32_t)t->tm_min  * 60U  +
         (uint32_t)t->tm_sec;
}

void tama_rtc_initial_sync(void)
{
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t) return;

  APP_LOG(APP_LOG_LEVEL_INFO,
          "RTC sync: initial -> Tama %02d:%02d:%02d",
          t->tm_hour, t->tm_min, t->tm_sec);

  write_time_to_tama((uint8_t)t->tm_hour,
                     (uint8_t)t->tm_min,
                     (uint8_t)t->tm_sec);

  s_initial_sync_done = true;
}

void tama_rtc_hourly_tick(struct tm *tick_time)
{
  if (!s_initial_sync_done) return;
  if (!tick_time) return;

  // Nur alle SYNC_INTERVAL_HOURS Stunden ausführen
  if ((tick_time->tm_hour % SYNC_INTERVAL_HOURS) != 0) return;

  uint32_t pebble_sec = seconds_since_midnight_from_tm(tick_time);
  uint32_t tama_sec   = read_tama_seconds_since_midnight();

  // Drift über Tagesgrenze hinweg sauber behandeln: min(diff, 86400-diff)
  uint32_t diff = (pebble_sec > tama_sec)
                  ? (pebble_sec - tama_sec)
                  : (tama_sec - pebble_sec);
  if (diff > 43200U) diff = 86400U - diff;

  if (diff <= DRIFT_TOLERANCE_SEC) {
    APP_LOG(APP_LOG_LEVEL_DEBUG,
            "RTC sync: drift %lu s <= toleranz, skip",
            (unsigned long)diff);
    return;
  }

  APP_LOG(APP_LOG_LEVEL_INFO,
          "RTC sync: drift %lu s -> resync auf %02d:%02d:%02d",
          (unsigned long)diff,
          tick_time->tm_hour, tick_time->tm_min, tick_time->tm_sec);

  write_time_to_tama((uint8_t)tick_time->tm_hour,
                     (uint8_t)tick_time->tm_min,
                     (uint8_t)tick_time->tm_sec);
}
