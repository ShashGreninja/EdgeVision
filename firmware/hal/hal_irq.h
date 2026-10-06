#ifndef HAL_IRQ_H
#define HAL_IRQ_H

#include <stdint.h>

/* Simulated interrupt lines. Numbers 0 and 1 are taken by the Windows port
 * (yield and tick); the rest are ours, up to 31. */
#define IRQ_MOTION      ( 2UL ) /* GPIO: motion pin raised (Ring event / PIR). */
#define IRQ_DMA_DONE    ( 3UL ) /* Camera DMA: frame transfer complete.        */
#define IRQ_NPU_DONE    ( 4UL ) /* Neural accelerator: inference job finished. */
#define IRQ_LINK_RX     ( 5UL ) /* Wi-Fi module link: bytes received or cable changed. */

#ifdef __cplusplus
extern "C" {
#endif

/* Raise an interrupt from a virtual-hardware Windows thread (FreeRTOS Windows port). */
void vPortGenerateSimulatedInterruptFromWindowsThread( uint32_t ulInterruptNumber );

#ifdef __cplusplus
}
#endif

#endif /* HAL_IRQ_H */
