#include "timing.h"

volatile uint32_t msTicks = 0;
static uint32_t core_mhz = 120U;

void SysTick_Handler(void)
{
    msTicks++;
}

void timing_init(void)
{
    SystemCoreClockUpdate();
    SysTick_Config(SystemCoreClock / 1000U);

    core_mhz = SystemCoreClock / 1000000U;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t millis(void)
{
    return msTicks;
}

void delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * core_mhz;

    while ((DWT->CYCCNT - start) < ticks)
    {
        /* wait */
    }
}

uint32_t timing_get_core_mhz(void)
{
    return core_mhz;
}
