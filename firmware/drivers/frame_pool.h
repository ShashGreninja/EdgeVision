#ifndef FRAME_POOL_H
#define FRAME_POOL_H

/* Static pool of frame buffers, allocated once at start-up.
 *
 * Every buffer has exactly one owner at a time, and ownership moves through
 * a fixed cycle:
 *
 *   FREE -> FILLING (CameraTask claims it, DMA writes into it)
 *        -> READY   (DMA complete, waiting in the frame queue)
 *        -> PROCESSING (a pipeline task is reading it)
 *        -> FREE
 *
 * A buffer can also go back to FREE early (DMA aborted, queue full, frame
 * rejected). Illegal transitions trip configASSERT. */

#include <stdint.h>

#include "FreeRTOS.h"

#define FRAME_POOL_SLOTS    3

typedef enum
{
    BUF_FREE = 0,
    BUF_FILLING,
    BUF_READY,
    BUF_PROCESSING
} buf_state_t;

typedef struct
{
    uint8_t * data;         /* FRAME_BYTES of grayscale pixels. */
    buf_state_t state;
    uint32_t seq;           /* Sensor sequence number of the frame. */
    TickType_t captured_at; /* Tick when DMA completed. */
} frame_buf_t;

/* Allocate all slots. Returns 0 on success. */
int frame_pool_init( void );

/* FREE -> FILLING. Blocks up to `wait` ticks for a slot to be released.
 * Returns the slot index, or -1 if every slot stayed busy. */
int frame_pool_claim( TickType_t wait );

/* Move a slot from `from` to `to`, asserting that it really was in `from`. */
void frame_pool_move( int idx, buf_state_t from, buf_state_t to );

frame_buf_t * frame_pool_get( int idx );

/* One letter per slot: F(ree) W(riting) R(eady) P(rocessing). out needs FRAME_POOL_SLOTS + 1 bytes. */
void frame_pool_snapshot( char * out );

#endif /* FRAME_POOL_H */
