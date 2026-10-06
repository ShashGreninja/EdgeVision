#include "services/network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "common/log.h"
#include "hal/hal_irq.h"
#include "hal/hal_link.h"

#define NETWORK_STACK_WORDS    ( configMINIMAL_STACK_SIZE * 2 )
#define EVENT_QUEUE_LEN        4
#define LINE_MAX               128
#define TX_LINE_MAX            ( NET_JSON_MAX + 96 )
#define DEVICE_ID_MAX          32

typedef struct
{
    uint32_t id;
    uint16_t len;
    char json[ NET_JSON_MAX ];
} stored_event_t;

static TaskHandle_t s_task;
static QueueHandle_t s_events;
static SemaphoreHandle_t s_tx_lock; /* Serialises writers of the link. */
static network_stats_t s_stats;
static char s_device[ DEVICE_ID_MAX ];
static char s_topic[ DEVICE_ID_MAX + 24 ];

/* Store-and-forward ring, allocated once from the RTOS heap. */
static stored_event_t * s_store;
static uint32_t s_head; /* Oldest event. */
static uint32_t s_count;
static uint32_t s_next_id = 1;

/* Publish in flight (one at a time keeps events in order). */
static int s_in_flight;
static TickType_t s_sent_at;
static TickType_t s_retry_after;

static char s_rx_line[ LINE_MAX ];
static size_t s_rx_len;

static uint32_t prvLinkIsr( void )
{
    BaseType_t woken = pdFALSE;

    vTaskNotifyGiveFromISR( s_task, &woken );
    return ( uint32_t ) woken;
}

static int prvSendLine( const char * line, size_t len )
{
    int ok;

    xSemaphoreTake( s_tx_lock, portMAX_DELAY );
    ok = ( hal_link_write( line, len ) == len );
    xSemaphoreGive( s_tx_lock );
    return ok;
}

static void prvStore( const event_t * ev )
{
    stored_event_t * slot;

    if( s_count == NET_STORE_LEN )
    {
        /* Full: the oldest event makes room for the newest. */
        if( s_in_flight && ( s_store[ s_head ].id == ( uint32_t ) s_in_flight ) )
        {
            s_in_flight = 0;
        }

        s_head = ( s_head + 1 ) % NET_STORE_LEN;
        s_count--;
        s_stats.dropped++;
    }

    slot = &s_store[ ( s_head + s_count ) % NET_STORE_LEN ];
    slot->id = s_next_id++;
    slot->len = ( uint16_t ) snprintf( slot->json, NET_JSON_MAX,
                                       "{\"device\":\"%s\",\"event\":\"%s\",\"confidence\":%.2f,\"zone\":\"front_door\","
                                       "\"seq\":%lu,\"box\":[%u,%u,%u,%u],\"uptime_ms\":%lu,\"id\":%lu}",
                                       s_device, ev->name, ( double ) ev->confidence, ( unsigned long ) ev->seq,
                                       ev->box.x, ev->box.y, ev->box.w, ev->box.h,
                                       ( unsigned long ) ev->uptime_ms, ( unsigned long ) slot->id );
    s_count++;
    s_stats.queued = s_count;
}

static void prvHandleLine( const char * line )
{
    if( strcmp( line, "+LINK:UP" ) == 0 )
    {
        if( !s_stats.online )
        {
            LOG( "[network] cloud reachable%s", s_count ? ", sending queued events" : "" );
        }

        s_stats.online = 1;
    }
    else if( strcmp( line, "+LINK:DOWN" ) == 0 )
    {
        if( s_stats.online )
        {
            LOG( "[network] cloud unreachable: detecting locally, queueing events" );
        }

        s_stats.online = 0;
    }
    else if( strncmp( line, "+PUBACK:", 8 ) == 0 )
    {
        const uint32_t id = ( uint32_t ) strtoul( line + 8, NULL, 10 );
        const char * result = strchr( line, ',' );

        if( !s_in_flight || ( id != ( uint32_t ) s_in_flight ) || ( s_count == 0 ) )
        {
            return; /* Stale acknowledgement. */
        }

        if( ( result != NULL ) && ( strcmp( result + 1, "OK" ) == 0 ) )
        {
            s_stats.published++;
            s_stats.bytes_up += s_store[ s_head ].len + ( uint32_t ) strlen( s_topic );
            s_head = ( s_head + 1 ) % NET_STORE_LEN;
            s_count--;
            s_stats.queued = s_count;
        }
        else
        {
            s_retry_after = xTaskGetTickCount() + pdMS_TO_TICKS( NET_RETRY_MS );
        }

        s_in_flight = 0;
    }
}

static void prvDrainRx( void )
{
    char buf[ 256 ];
    size_t n;

    while( ( n = hal_link_read( buf, sizeof( buf ) ) ) > 0 )
    {
        for( size_t i = 0; i < n; i++ )
        {
            if( ( buf[ i ] == '\n' ) || ( buf[ i ] == '\r' ) )
            {
                if( s_rx_len > 0 )
                {
                    s_rx_line[ s_rx_len ] = '\0';
                    prvHandleLine( s_rx_line );
                    s_rx_len = 0;
                }
            }
            else if( s_rx_len < LINE_MAX - 1 )
            {
                s_rx_line[ s_rx_len++ ] = buf[ i ];
            }
        }
    }
}

static void prvPumpPublish( void )
{
    const TickType_t now = xTaskGetTickCount();
    char line[ TX_LINE_MAX ];

    if( s_in_flight && ( ( now - s_sent_at ) >= pdMS_TO_TICKS( NET_ACK_TIMEOUT_MS ) ) )
    {
        s_in_flight = 0; /* No answer: send it again. */
        s_stats.retries++;
    }

    if( s_in_flight || ( s_count == 0 ) || !s_stats.online || !s_stats.cable ||
        ( ( int32_t ) ( now - s_retry_after ) < 0 ) )
    {
        return;
    }

    const stored_event_t * ev = &s_store[ s_head ];
    const int len = snprintf( line, sizeof( line ), "AT+PUB=%lu,%s,%s\n", ( unsigned long ) ev->id, s_topic, ev->json );

    if( ( len > 0 ) && ( len < ( int ) sizeof( line ) ) && prvSendLine( line, ( size_t ) len ) )
    {
        s_in_flight = ( int ) ev->id;
        s_sent_at = now;
    }
}

static void prvNetworkTask( void * param )
{
    ( void ) param;

    for( ; ; )
    {
        event_t ev;

        /* Wake for new events, link interrupts, or at least every 100 ms. */
        if( xQueueReceive( s_events, &ev, pdMS_TO_TICKS( 100 ) ) == pdPASS )
        {
            prvStore( &ev );
        }

        ( void ) ulTaskNotifyTake( pdTRUE, 0 );

        const uint8_t cable = ( uint8_t ) hal_link_connected();

        if( cable != s_stats.cable )
        {
            s_stats.cable = cable;
            s_stats.online = 0; /* Wait for the module to report +LINK:UP. */
            s_in_flight = 0;
            s_rx_len = 0;
        }

        prvDrainRx();
        prvPumpPublish();
    }
}

int network_init( const char * device_id, int link_port )
{
    snprintf( s_device, sizeof( s_device ), "%s", device_id );
    snprintf( s_topic, sizeof( s_topic ), "edgevision/%s/events", s_device );

    s_events = xQueueCreate( EVENT_QUEUE_LEN, sizeof( event_t ) );
    s_tx_lock = xSemaphoreCreateMutex();
    s_store = pvPortMalloc( sizeof( stored_event_t ) * NET_STORE_LEN );

    if( ( s_events == NULL ) || ( s_tx_lock == NULL ) || ( s_store == NULL ) ||
        ( xTaskCreate( prvNetworkTask, "Network", NETWORK_STACK_WORDS, NULL, PRIO_NETWORK, &s_task ) != pdPASS ) )
    {
        return -1;
    }

    vQueueAddToRegistry( s_events, "EventQ" );
    vPortSetInterruptHandler( IRQ_LINK_RX, prvLinkIsr );

    if( link_port > 0 )
    {
        hal_link_init( link_port );
    }

    return 0;
}

void network_post_event( const event_t * ev )
{
    if( xQueueSend( s_events, ev, 0 ) != pdPASS )
    {
        s_stats.dropped++;
    }
}

void network_send_telemetry( const char * json )
{
    char line[ 1024 ];
    const int len = snprintf( line, sizeof( line ), "AT+TEL=%s\n", json );

    if( s_stats.cable && ( len > 0 ) && ( len < ( int ) sizeof( line ) ) && prvSendLine( line, ( size_t ) len ) )
    {
        s_stats.telemetry_sent++;
    }
}

void network_simulate_outage( uint32_t seconds )
{
    char line[ 32 ];
    const int len = snprintf( line, sizeof( line ), "AT+NETDOWN=%lu\n", ( unsigned long ) seconds );

    ( void ) prvSendLine( line, ( size_t ) len );
}

void network_get_stats( network_stats_t * out )
{
    taskENTER_CRITICAL();
    *out = s_stats;
    taskEXIT_CRITICAL();
}

const char * network_device_id( void )
{
    return s_device;
}
