#include "uart1_drv.h"

/* UART1 */
#define UART_RX_PIN     3
#define UART_TX_PIN     4

volatile char rx_buf[RX_BUF_SIZE];
volatile uint16_t rx_idx = 0;
volatile bool line_ready = false;

void uart1_init(void)
{
    SIM->SCGC4 |= SIM_SCGC4_UART1_MASK;
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK;

    PORTC->PCR[UART_TX_PIN] = PORT_PCR_MUX(3);
    PORTC->PCR[UART_RX_PIN] = PORT_PCR_MUX(3);

    UART1->C2 = 0;
    UART1->BDH = 0;
    UART1->BDL = 11;
    UART1->C4  = 3;
    UART1->C1 = 0;

    UART1->C2 = UART_C2_TE_MASK | UART_C2_RE_MASK | UART_C2_RIE_MASK;

    NVIC_EnableIRQ(UART1_RX_TX_IRQn);
}

void UART1_RX_TX_IRQHandler(void)
{
    if (UART1->S1 & UART_S1_RDRF_MASK)
    {
        char c = (char)UART1->D;

        if (line_ready)
        {
            return;
        }

        if (c == '\n')
        {
            rx_buf[rx_idx] = '\0';
            line_ready = true;
        }
        else if (c != '\r')
        {
            if (rx_idx < (RX_BUF_SIZE - 1U))
            {
                rx_buf[rx_idx++] = c;
            }
        }
    }
}

void uart1_puts(const char *str)
{
    while (*str)
    {
        while (!(UART1->S1 & UART_S1_TDRE_MASK))
        {
            /* wait */
        }

        UART1->D = (uint8_t)(*str++);
    }
}
