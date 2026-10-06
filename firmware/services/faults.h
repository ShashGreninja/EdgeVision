#ifndef FAULTS_H
#define FAULTS_H

/* FaultTask: injects faults on command, to show that every failure has a
 * detection path and a recovery path. Commands arrive on IRQ_FAULT from the
 * debug inputs ("FAULT <name>" over UDP, keys 1-7).
 *
 *   camera_disconnect  sensor stops for 3 s     -> DMA timeouts -> sensor reset with backoff
 *   frame_corrupt      next 5 transfers garbled  -> CRC mismatch -> frames dropped
 *   mem_pressure       a frame slot lent away 5 s -> pipeline runs on 2 slots, then restored
 *   inference_hang     NPU stops answering       -> watchdog restarts InferenceTask, resets NPU
 *   network_down       Wi-Fi module offline 10 s -> events queued, sent in order afterwards
 *   cpu_overload       costs x4 for 5 s          -> governor drops more frames, stays real-time
 *   selftest           test event through the network path */

#include <stdint.h>

#include "common/fault_ids.h"

typedef struct
{
    uint32_t injected[ FAULT_COUNT ];
    uint8_t mem_pressure_active;
    uint8_t cpu_overload_active;
} faults_stats_t;

int faults_init( void );

void faults_get_stats( faults_stats_t * out );

#endif /* FAULTS_H */
