#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

/* TelemetryTask: once a second, prints what every part of the pipeline did
 * in the last second, the frame pool's state and RAM usage.
 * If run_seconds > 0, prints a summary and exits after that many seconds. */
int telemetry_init( uint32_t run_seconds );

#endif /* TELEMETRY_H */
