#include "drivers/frame_pool.h"

#include "hal/hal_sensor.h"
#include "semphr.h"

static frame_buf_t s_slots[ FRAME_POOL_SLOTS ];
static SemaphoreHandle_t s_lock;       /* Guards slot states.          */
static SemaphoreHandle_t s_free_slots; /* Counts slots in BUF_FREE.    */

int frame_pool_init( void )
{
    s_lock = xSemaphoreCreateMutex();
    s_free_slots = xSemaphoreCreateCounting( FRAME_POOL_SLOTS, FRAME_POOL_SLOTS );

    if( ( s_lock == NULL ) || ( s_free_slots == NULL ) )
    {
        return -1;
    }

    for( int i = 0; i < FRAME_POOL_SLOTS; i++ )
    {
        /* Allocated from the 512 KB FreeRTOS heap so it counts against the budget. */
        s_slots[ i ].data = pvPortMalloc( FRAME_BYTES );

        if( s_slots[ i ].data == NULL )
        {
            return -1;
        }

        s_slots[ i ].state = BUF_FREE;
    }

    return 0;
}

int frame_pool_claim( TickType_t wait )
{
    int idx = -1;

    /* Sleep until a slot is released (or the wait expires). */
    if( xSemaphoreTake( s_free_slots, wait ) != pdTRUE )
    {
        return -1;
    }

    xSemaphoreTake( s_lock, portMAX_DELAY );

    for( int i = 0; i < FRAME_POOL_SLOTS; i++ )
    {
        if( s_slots[ i ].state == BUF_FREE )
        {
            s_slots[ i ].state = BUF_FILLING;
            idx = i;
            break;
        }
    }

    xSemaphoreGive( s_lock );
    return idx;
}

void frame_pool_move( int idx, buf_state_t from, buf_state_t to )
{
    configASSERT( ( idx >= 0 ) && ( idx < FRAME_POOL_SLOTS ) );

    configASSERT( from != BUF_FREE ); /* Leaving FREE goes through frame_pool_claim(). */

    xSemaphoreTake( s_lock, portMAX_DELAY );
    configASSERT( s_slots[ idx ].state == from );
    s_slots[ idx ].state = to;
    xSemaphoreGive( s_lock );

    if( to == BUF_FREE )
    {
        xSemaphoreGive( s_free_slots );
    }
}

frame_buf_t * frame_pool_get( int idx )
{
    configASSERT( ( idx >= 0 ) && ( idx < FRAME_POOL_SLOTS ) );
    return &s_slots[ idx ];
}

int frame_pool_reserve( TickType_t wait )
{
    const int idx = frame_pool_claim( wait );

    if( idx >= 0 )
    {
        frame_pool_move( idx, BUF_FILLING, BUF_RESERVED );
    }

    return idx;
}

void frame_pool_snapshot( char * out )
{
    static const char letters[] = { 'F', 'W', 'R', 'P', 'X' };

    xSemaphoreTake( s_lock, portMAX_DELAY );

    for( int i = 0; i < FRAME_POOL_SLOTS; i++ )
    {
        out[ i ] = letters[ s_slots[ i ].state ];
    }

    xSemaphoreGive( s_lock );
    out[ FRAME_POOL_SLOTS ] = '\0';
}
