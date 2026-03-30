#ifndef PIR_H
#define PIR_H

#include <stdbool.h>
#include <stdint.h>

/* =========================================================
 * CORE
 * ========================================================= */

void pir_init(void);
uint8_t read_pir(void);
void pir_update(uint32_t now_ms);

/* =========================================================
 * EVENT / STATUS
 * ========================================================= */

bool pir_unusual_motion_event(void);
bool pir_peek_unusual_motion(void);
bool pir_is_unusual_active(uint32_t now_ms);

bool pir_light_hold_active(uint32_t now_ms);
void pir_force_light_hold(uint32_t hold_ms, uint32_t now_ms);

bool pir_is_motion_active(void);

/* =========================================================
 * ML / DASHBOARD HELPERS
 * ========================================================= */

uint32_t pir_get_motion_duration(uint32_t now_ms);
uint8_t pir_get_motion_frequency(void);
uint8_t pir_motion_intensity(void);

bool pir_is_simulated(void);

/* =========================================================
 * DEBUG
 * ========================================================= */

uint32_t pir_debug_current_high_ms(uint32_t now_ms);
uint32_t pir_debug_window_high_ms(uint32_t now_ms);
uint32_t pir_debug_cooldown_left(uint32_t now_ms);
uint8_t pir_debug_high_count(void);
bool pir_debug_stuck_high(uint32_t now_ms);

/* =========================================================
 * DIGITAL TWIN
 * ========================================================= */

void pir_simulate_motion_pulse(uint32_t now_ms, uint32_t duration_ms);
bool pir_simulation_active(void);

#endif
