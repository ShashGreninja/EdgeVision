#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

#define TELEMETRY_JSON_MAX    1536 /* A full snapshot is ~1 KB. */

/* TelemetryTask: once a second, prints what every part of the pipeline did
 * in the last second, the frame pool's state and RAM usage, and sends the
 * same snapshot as JSON to the Wi-Fi module for the dashboard.
 * If run_seconds > 0, prints "[done] <json>" and exits after that many seconds. */
int telemetry_init( uint32_t run_seconds );

#endif /* TELEMETRY_H */
