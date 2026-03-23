#include "MK66F18.h"
#include "system_ctrl.h"

// SW3 = PTA10, active low
#define SW3_PORT    PORTA
#define SW3_GPIO    GPIOA
#define SW3_PIN     10

// SW2 = PTD11, active low
#define SW2_PORT    PORTD
#define SW2_GPIO    GPIOD
#define SW2_PIN     11

void sw3_init(void) {
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;
    SW3_PORT->PCR[SW3_PIN] = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;
    SW3_GPIO->PDDR &= ~(1U << SW3_PIN);
}

uint8_t sw3_is_pressed(void) {
    return (((SW3_GPIO->PDIR >> SW3_PIN) & 0x01U) == 0U) ? 1U : 0U;
}

void sw2_init(void) {
    SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK;
    SW2_PORT->PCR[SW2_PIN] = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;
    SW2_GPIO->PDDR &= ~(1U << SW2_PIN);
}

uint8_t sw2_is_pressed(void) {
    return (((SW2_GPIO->PDIR >> SW2_PIN) & 0x01U) == 0U) ? 1U : 0U;
}
