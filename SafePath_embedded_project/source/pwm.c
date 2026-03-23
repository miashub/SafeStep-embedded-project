#include "MK66F18.h"
#include "pwm.h"

// PTD0 = FTM3_CH0
#define PWM_PORT        PORTD
#define PWM_PIN         0
#define PWM_FTM         FTM3
#define PWM_CHANNEL     0
#define PWM_PIN_MUX     4

// ~1 kHz PWM if clock = 120 MHz, prescaler = 128
#define PWM_MOD_VALUE   937U

// Dark threshold based on your measured LDR values
// Lower = darker, higher = brighter
#define LDR_DARK_THRESHOLD  2200U

static uint8_t current_duty = 255U;

void pwm_init(void) {
    // Enable clocks
    SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK;
    SIM->SCGC3 |= SIM_SCGC3_FTM3_MASK;

    // Select clock source for FTM/TPM modules
    SIM->SOPT2 = (SIM->SOPT2 & ~SIM_SOPT2_TPMSRC_MASK) | SIM_SOPT2_TPMSRC(1);

    // Set PTD0 to FTM3_CH0
    PWM_PORT->PCR[PWM_PIN] = PORT_PCR_MUX(PWM_PIN_MUX);

    // Disable write protection
    PWM_FTM->MODE |= FTM_MODE_WPDIS_MASK;

    // Disable counter before config
    PWM_FTM->SC = 0;
    PWM_FTM->CNT = 0;
    PWM_FTM->MOD = PWM_MOD_VALUE;

    // Edge-aligned, high-true PWM
    PWM_FTM->CONTROLS[PWM_CHANNEL].CnSC =
        FTM_CnSC_MSB_MASK | FTM_CnSC_ELSB_MASK;

    // Start with 0% duty
    PWM_FTM->CONTROLS[PWM_CHANNEL].CnV = 0;

    // System clock, prescaler /128
    PWM_FTM->SC = FTM_SC_CLKS(1) | FTM_SC_PS(7);
}

void pwm_set_duty_percent(uint8_t percent) {
    uint32_t cnv;

    if (percent > 100U) {
        percent = 100U;
    }

    if (percent == current_duty) {
        return;
    }

    cnv = ((uint32_t)(PWM_MOD_VALUE + 1U) * percent) / 100U;

    if (cnv > PWM_MOD_VALUE) {
        cnv = PWM_MOD_VALUE;
    }

    PWM_FTM->CONTROLS[PWM_CHANNEL].CnV = (uint16_t)cnv;
    current_duty = percent;
}

uint8_t pwm_is_dark(uint16_t ldr_value) {
    return (ldr_value <= LDR_DARK_THRESHOLD) ? 1U : 0U;
}

uint8_t pwm_get_duty_from_ldr(uint16_t ldr_value) {
    if (ldr_value <= 600U) {
        return 100U;
    } else if (ldr_value <= 900U) {
        return 80U;
    } else if (ldr_value <= 1200U) {
        return 60U;
    } else if (ldr_value <= 1600U) {
        return 40U;
    } else if (ldr_value <= 2200U) {
        return 20U;
    } else {
        return 0U;
    }
}

void pwm_update_from_sensors(uint16_t ldr_value, uint8_t pir_value) {
    if (pwm_is_dark(ldr_value) && pir_value) {
        pwm_set_duty_percent(pwm_get_duty_from_ldr(ldr_value));
    } else {
        pwm_set_duty_percent(0U);
    }
}

