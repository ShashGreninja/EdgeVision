#ifndef HAL_SENSOR_H
#define HAL_SENSOR_H

/* Virtual camera module: a 1080p/30 fps sensor with an on-module ISP that
 * outputs QVGA grayscale, plus a one-shot DMA channel.
 *
 * It runs as a Windows thread ("hardware"), outside the RTOS. The firmware
 * talks to it only the way it would talk to real hardware: power it on/off,
 * program the DMA destination, and receive IRQ_DMA_DONE when a frame lands.
 * If no DMA destination is armed when a frame is ready, the hardware drops
 * that frame and counts an overrun. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_NATIVE_W    1920
#define SENSOR_NATIVE_H    1080
#define SENSOR_FPS         30

/* ISP output format: QVGA, 8-bit grayscale. */
#define FRAME_W            320
#define FRAME_H            240
#define FRAME_BYTES        ( FRAME_W * FRAME_H )

typedef struct
{
    uint32_t produced;    /* Frames captured by the sensor while powered.   */
    uint32_t transferred; /* Frames copied into firmware memory by DMA.     */
    uint32_t overruns;    /* Frames dropped: no DMA destination was armed.  */
} hal_sensor_stats_t;

/* Start the sensor thread (powered off). clip_path may be NULL for a synthetic scene. */
int hal_sensor_init( const char * clip_path );

void hal_sensor_set_power( int on );

/* Program the DMA channel to write the next frame into dst (FRAME_BYTES long). */
void hal_sensor_dma_arm( uint8_t * dst );

/* Cancel an armed transfer. Returns 1 if it was cancelled, 0 if it had already completed. */
int hal_sensor_dma_abort( void );

/* Sequence number of the most recent completed transfer. */
uint32_t hal_sensor_last_seq( void );

void hal_sensor_get_stats( hal_sensor_stats_t * out );

#ifdef __cplusplus
}
#endif

#endif /* HAL_SENSOR_H */
