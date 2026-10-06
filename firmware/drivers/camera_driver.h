#ifndef CAMERA_DRIVER_H
#define CAMERA_DRIVER_H

/* Camera driver + CameraTask.
 *
 * Sleeps with the sensor powered off until the MOTION interrupt fires, then
 * streams: claim a free buffer, arm the DMA, wait for IRQ_DMA_DONE, hand the
 * buffer index to the frame queue. After QUIET_TIMEOUT_MS without motion it
 * powers the sensor off again. */

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

typedef struct
{
    uint32_t motion_irqs;     /* MOTION interrupts received.                   */
    uint32_t wakeups;         /* Idle -> streaming transitions.                */
    uint32_t frames_captured; /* Frames that reached READY.                    */
    uint32_t no_buffer;       /* Times no FREE slot was available to arm DMA.  */
    uint32_t dma_timeouts;    /* DMA did not complete within DMA_TIMEOUT_MS.   */
    uint32_t queue_drops;     /* READY frames dropped because the queue was full. */
    uint32_t corrupt_frames;  /* Frames whose CRC did not match: dropped.  */
    uint32_t sensor_lost;     /* Times the sensor stopped responding.      */
    uint32_t sensor_resets;   /* Sensor power-cycles during recovery.      */
} camera_stats_t;

/* Create CameraTask, the frame queue and the interrupt handlers.
 * Returns the frame queue (items are int slot indices), or NULL on failure. */
QueueHandle_t camera_driver_init( void );

void camera_get_stats( camera_stats_t * out );

/* Called by the pipeline while something is confirmed in view: keeps the
 * camera streaming as if the motion pin had fired again. */
void camera_note_activity( void );

int camera_is_streaming( void );

/* Total time the sensor has been powered and streaming, in ms. */
uint32_t camera_awake_ms( void );

#endif /* CAMERA_DRIVER_H */
