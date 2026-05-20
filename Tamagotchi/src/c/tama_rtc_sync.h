/*
 * tama_rtc_sync.h
 *
 * Synchronisiert die Pebble-Systemzeit (RTC) mit der Tamagotchi-internen
 * Uhr, die im emulierten 4-bit RAM von tamalib liegt.
 *
 * Memory-Map (Tama-Adressen, getestet):
 *   0x010: Sekunden Einer (BCD low nibble)
 *   0x011: Sekunden Zehner (BCD low nibble)
 *   0x012: Minuten Einer (BCD low nibble)
 *   0x013: Minuten Zehner (BCD low nibble)
 *   0x014: Stunden Low Nibble (HEX! nicht BCD)
 *   0x015: Stunden High Nibble (HEX)
 *
 * Wichtig: Die Stundenfelder sind HEX-genibbled, NICHT BCD. Bei 21 Uhr:
 *   hr_l = 21 & 0x0F = 5
 *   hr_h = (21 >> 4) & 0x0F = 1
 * Tama rekonstruiert: (hr_h << 4) | hr_l = 0x15 = 21 ✓
 *
 * Schreibzugriff erfolgt über die SET_MEMORY-Makros aus cpu.h, damit
 * das Nibble-Packing im LOW_FOOTPRINT-Modus korrekt funktioniert.
 */

#ifndef TAMA_RTC_SYNC_H
#define TAMA_RTC_SYNC_H

#include <pebble.h>

// Initialer Sync: schreibt die aktuelle Pebble-Zeit in den Tama-RAM
// und ruft cpu_sync_ref_timestamp() auf. Wird beim App-Start nach
// tamalib_init / cpu_init_from_state aufgerufen.
void tama_rtc_initial_sync(void);

// Auto-Sync-Tick (tick_timer_service Variante): prüft Drift und syncs
// nur bei H % 2 == 0. Wird vom HOUR_UNIT-Tick aufgerufen.
void tama_rtc_hourly_tick(struct tm *tick_time);

// Auto-Sync (AppTimer Variante): prüft jeden Aufruf die Drift und syncs
// bei Bedarf, unabhängig von der Tageszeit. Einfacher und robuster als
// tick_timer_service. Aufrufer registriert dazu einen periodischen AppTimer.
void tama_rtc_periodic_check(void);

#endif // TAMA_RTC_SYNC_H
