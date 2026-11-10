/*
 * startup.c - Vector table and reset handler for the bootloader.
 */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void SysTick_Handler(void);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; dst++) *dst = 0;
    main();
    for (;;) { }
}

void Default_Handler(void)
{
    /* Unexpected fault or interrupt: reset and try again. */
    *(volatile uint32_t *)0xE000ED0CUL = 0x05FA0004UL;   /* SCB->AIRCR SYSRESETREQ */
    for (;;) { }
}

typedef void (*vector_t)(void);

/* 16 Cortex-M0+ system vectors + 32 STM32G0 interrupts. */
__attribute__((section(".isr_vector"), used))
const vector_t g_vectors[48] = {
    (vector_t)(uintptr_t)&_estack,
    Reset_Handler,
    Default_Handler,   /* NMI       */
    Default_Handler,   /* HardFault */
    0, 0, 0, 0, 0, 0, 0,
    Default_Handler,   /* SVCall    */
    0, 0,
    Default_Handler,   /* PendSV    */
    SysTick_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
};
