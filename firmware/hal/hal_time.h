#ifndef HAL_TIME_H
#define HAL_TIME_H

#include <stdint.h>

/* Free-running microsecond counter, the simulator's equivalent of a
 * hardware cycle counter (e.g. DWT->CYCCNT on a Cortex-M). Used to measure
 * how long a pipeline stage took. */
uint64_t hal_time_us( void );

#endif /* HAL_TIME_H */
