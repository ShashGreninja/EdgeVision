/* Unit tests for the frame pool, running on the FreeRTOS simulator.
 *
 * Usage: edgevision_tests                 run all tests, exit 0 if they pass
 *        edgevision_tests --illegal-move  make an illegal state change (must assert, exit 2)
 *        edgevision_tests --illegal-free  leave FREE without claiming   (must assert, exit 2)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "common/log.h"
#include "drivers/frame_pool.h"
#include "hal/hal_sensor.h"

static int s_checks;
static int s_failures;
static const char * s_mode = "";

#define CHECK( cond )                                                          \
    do {                                                                       \
        s_checks++;                                                            \
        if( !( cond ) )                                                        \
        {                                                                      \
            s_failures++;                                                      \
            LOG( "  FAIL %s:%d: %s", __FILE__, __LINE__, #cond );              \
        }                                                                      \
    } while( 0 )

static char prvSlotLetter( int idx )
{
    char snap[ FRAME_POOL_SLOTS + 1 ];

    frame_pool_snapshot( snap );
    return snap[ idx ];
}

static void prvExpectSnapshot( const char * expected )
{
    char snap[ FRAME_POOL_SLOTS + 1 ];

    frame_pool_snapshot( snap );
    CHECK( strcmp( snap, expected ) == 0 );
}

/*----------------------------------- Tests ------------------------------------*/

static void test_initial_state( void )
{
    const size_t used = configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize();

    prvExpectSnapshot( "FFF" );

    for( int i = 0; i < FRAME_POOL_SLOTS; i++ )
    {
        CHECK( frame_pool_get( i )->data != NULL );

        for( int j = i + 1; j < FRAME_POOL_SLOTS; j++ )
        {
            CHECK( frame_pool_get( i )->data != frame_pool_get( j )->data );
        }
    }

    /* The slots come out of the 512 KB RTOS heap, so they count against the budget. */
    CHECK( used >= ( size_t ) FRAME_POOL_SLOTS * FRAME_BYTES );
}

static void test_claim_until_exhausted( void )
{
    int got[ FRAME_POOL_SLOTS ];
    TickType_t start;

    for( int i = 0; i < FRAME_POOL_SLOTS; i++ )
    {
        got[ i ] = frame_pool_claim( 0 );
        CHECK( ( got[ i ] >= 0 ) && ( got[ i ] < FRAME_POOL_SLOTS ) );
        CHECK( prvSlotLetter( got[ i ] ) == 'W' );
    }

    CHECK( ( got[ 0 ] != got[ 1 ] ) && ( got[ 1 ] != got[ 2 ] ) && ( got[ 0 ] != got[ 2 ] ) );
    prvExpectSnapshot( "WWW" );

    /* Pool empty: an immediate claim fails... */
    CHECK( frame_pool_claim( 0 ) == -1 );

    /* ...and a timed claim fails only after waiting the full time. */
    start = xTaskGetTickCount();
    CHECK( frame_pool_claim( pdMS_TO_TICKS( 50 ) ) == -1 );
    CHECK( ( xTaskGetTickCount() - start ) >= pdMS_TO_TICKS( 50 ) );

    for( int i = 0; i < FRAME_POOL_SLOTS; i++ )
    {
        frame_pool_move( got[ i ], BUF_FILLING, BUF_FREE );
    }

    prvExpectSnapshot( "FFF" );
}

static void test_full_cycle( void )
{
    const int idx = frame_pool_claim( 0 );

    CHECK( idx >= 0 );
    CHECK( prvSlotLetter( idx ) == 'W' );
    frame_pool_move( idx, BUF_FILLING, BUF_READY );
    CHECK( prvSlotLetter( idx ) == 'R' );
    frame_pool_move( idx, BUF_READY, BUF_PROCESSING );
    CHECK( prvSlotLetter( idx ) == 'P' );
    frame_pool_move( idx, BUF_PROCESSING, BUF_FREE );
    CHECK( prvSlotLetter( idx ) == 'F' );

    /* A slot that went round the cycle can be claimed again. */
    const int again = frame_pool_claim( 0 );

    CHECK( again >= 0 );
    frame_pool_move( again, BUF_FILLING, BUF_FREE );
    prvExpectSnapshot( "FFF" );
}

static void test_early_release_paths( void )
{
    /* DMA aborted: FILLING -> FREE. */
    int idx = frame_pool_claim( 0 );

    frame_pool_move( idx, BUF_FILLING, BUF_FREE );
    CHECK( prvSlotLetter( idx ) == 'F' );

    /* Frame queue full: READY -> FREE. */
    idx = frame_pool_claim( 0 );
    frame_pool_move( idx, BUF_FILLING, BUF_READY );
    frame_pool_move( idx, BUF_READY, BUF_FREE );
    CHECK( prvSlotLetter( idx ) == 'F' );

    /* After both, all three slots are claimable again. */
    for( int i = 0; i < FRAME_POOL_SLOTS; i++ )
    {
        CHECK( frame_pool_claim( 0 ) >= 0 );
    }

    for( int i = 0; i < FRAME_POOL_SLOTS; i++ )
    {
        frame_pool_move( i, BUF_FILLING, BUF_FREE );
    }

    prvExpectSnapshot( "FFF" );
}

static volatile int s_release_idx;

static void prvReleaseLater( void * param )
{
    ( void ) param;
    vTaskDelay( pdMS_TO_TICKS( 30 ) );
    frame_pool_move( s_release_idx, BUF_FILLING, BUF_FREE );
    vTaskDelete( NULL );
}

static void test_claim_blocks_until_release( void )
{
    int got[ FRAME_POOL_SLOTS ];
    TickType_t start, waited;

    for( int i = 0; i < FRAME_POOL_SLOTS; i++ )
    {
        got[ i ] = frame_pool_claim( 0 );
    }

    /* Another task frees the middle slot 30 ms from now. */
    s_release_idx = got[ 1 ];
    CHECK( xTaskCreate( prvReleaseLater, "Releaser", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL ) == pdPASS );

    start = xTaskGetTickCount();
    const int idx = frame_pool_claim( pdMS_TO_TICKS( 500 ) );
    waited = xTaskGetTickCount() - start;

    CHECK( idx == got[ 1 ] );
    CHECK( waited >= pdMS_TO_TICKS( 25 ) );
    CHECK( waited < pdMS_TO_TICKS( 250 ) );

    for( int i = 0; i < FRAME_POOL_SLOTS; i++ )
    {
        frame_pool_move( got[ i ], BUF_FILLING, BUF_FREE );
    }

    prvExpectSnapshot( "FFF" );
}

/*--------------------------------- Runner -------------------------------------*/

static void prvRun( const char * name, void ( * fn )( void ) )
{
    const int before = s_failures;

    fn();
    LOG( "[unit] %-34s %s", name, ( s_failures == before ) ? "ok" : "FAILED" );
}

static void prvTestTask( void * param )
{
    ( void ) param;

    if( strcmp( s_mode, "--illegal-move" ) == 0 )
    {
        const int idx = frame_pool_claim( 0 );

        LOG( "[unit] moving slot %d READY->PROCESSING while it is FILLING (must assert)", idx );
        frame_pool_move( idx, BUF_READY, BUF_PROCESSING );
        LOG( "[unit] illegal move was NOT caught" );
        exit( 0 );
    }

    if( strcmp( s_mode, "--illegal-free" ) == 0 )
    {
        LOG( "[unit] moving slot 0 FREE->READY without claiming (must assert)" );
        frame_pool_move( 0, BUF_FREE, BUF_READY );
        LOG( "[unit] illegal move was NOT caught" );
        exit( 0 );
    }

    prvRun( "initial state", test_initial_state );
    prvRun( "claim until exhausted", test_claim_until_exhausted );
    prvRun( "full ownership cycle", test_full_cycle );
    prvRun( "early release paths", test_early_release_paths );
    prvRun( "claim blocks until release", test_claim_blocks_until_release );

    LOG( "[unit] %d checks, %d failures", s_checks, s_failures );
    exit( ( s_failures == 0 ) ? 0 : 1 );
}

int main( int argc, char ** argv )
{
    if( argc > 1 )
    {
        s_mode = argv[ 1 ];
    }

    if( frame_pool_init() != 0 )
    {
        printf( "[unit] frame_pool_init failed\n" );
        return 1;
    }

    xTaskCreate( prvTestTask, "Tests", configMINIMAL_STACK_SIZE * 4, NULL, tskIDLE_PRIORITY + 2, NULL );
    vTaskStartScheduler();
    return 1;
}

/*------------------------ Hooks required by the config ------------------------*/

void ev_trace_malloc( size_t xSize )
{
    ( void ) xSize;
}

void vApplicationMallocFailedHook( void )
{
    printf( "[fatal] out of RAM\n" );
    fflush( stdout );
    exit( 1 );
}

void vAssertCalled( const char * pcFile, unsigned long ulLine )
{
    printf( "[fatal] assert failed at %s:%lu\n", pcFile, ulLine );
    fflush( stdout );
    exit( 2 );
}
