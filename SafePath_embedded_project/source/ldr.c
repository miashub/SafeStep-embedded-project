#include "MK66F18.h"
#include "ldr.h"

#define LDR_PORT    PORTB
#define LDR_PIN     6
#define LDR_ADC     ADC1
#define LDR_ADC_CH  12

void ldr_init(void) {
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;
    LDR_PORT->PCR[LDR_PIN] = PORT_PCR_MUX(0);
}

uint16_t read_ldr(void) {
    LDR_ADC->SC1[0] = LDR_ADC_CH;

    while (!(LDR_ADC->SC1[0] & ADC_SC1_COCO_MASK));

    return LDR_ADC->R[0];
}
