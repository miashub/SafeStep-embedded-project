#include <board_led.h>

/* LEDs */
#define LED_RED_PORT    PORTC
#define LED_RED_GPIO    GPIOC
#define LED_RED_PIN     9

#define LED_GREEN_PORT  PORTE
#define LED_GREEN_GPIO  GPIOE
#define LED_GREEN_PIN   6

#define LED_BLUE_PORT   PORTA
#define LED_BLUE_GPIO   GPIOA
#define LED_BLUE_PIN    11

void led_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK | SIM_SCGC5_PORTE_MASK | SIM_SCGC5_PORTA_MASK;

    LED_RED_PORT->PCR[LED_RED_PIN]     = PORT_PCR_MUX(1);
    LED_GREEN_PORT->PCR[LED_GREEN_PIN] = PORT_PCR_MUX(1);
    LED_BLUE_PORT->PCR[LED_BLUE_PIN]   = PORT_PCR_MUX(1);

    LED_RED_GPIO->PDDR   |= (1U << LED_RED_PIN);
    LED_GREEN_GPIO->PDDR |= (1U << LED_GREEN_PIN);
    LED_BLUE_GPIO->PDDR  |= (1U << LED_BLUE_PIN);

    /* Active-low onboard LEDs: set high = OFF */
    LED_RED_GPIO->PSOR   = (1U << LED_RED_PIN);
    LED_GREEN_GPIO->PSOR = (1U << LED_GREEN_PIN);
    LED_BLUE_GPIO->PSOR  = (1U << LED_BLUE_PIN);
}

void led_set(int r, int g, int b)
{
    if (r) { LED_RED_GPIO->PCOR = (1U << LED_RED_PIN); }
    else   { LED_RED_GPIO->PSOR = (1U << LED_RED_PIN); }

    if (g) { LED_GREEN_GPIO->PCOR = (1U << LED_GREEN_PIN); }
    else   { LED_GREEN_GPIO->PSOR = (1U << LED_GREEN_PIN); }

    if (b) { LED_BLUE_GPIO->PCOR = (1U << LED_BLUE_PIN); }
    else   { LED_BLUE_GPIO->PSOR = (1U << LED_BLUE_PIN); }
}
