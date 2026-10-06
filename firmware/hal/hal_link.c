#include <winsock2.h>
#include <windows.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "hal/hal_irq.h"
#include "hal/hal_link.h"

/* Single-producer / single-consumer byte rings. TX: firmware writes, link
 * thread reads. RX: link thread writes, firmware reads. Indices only grow;
 * position = index % size. */
#define RING_SIZE    8192u

typedef struct
{
    char buf[ RING_SIZE ];
    atomic_uint head; /* Next write index. */
    atomic_uint tail; /* Next read index.  */
} ring_t;

static ring_t s_tx;
static ring_t s_rx;
static atomic_int s_connected;
static int s_port;

static size_t prvRingFree( ring_t * r )
{
    return RING_SIZE - ( atomic_load( &r->head ) - atomic_load( &r->tail ) );
}

static size_t prvRingUsed( ring_t * r )
{
    return atomic_load( &r->head ) - atomic_load( &r->tail );
}

static void prvRingPut( ring_t * r, const char * data, size_t len )
{
    unsigned head = atomic_load( &r->head );

    for( size_t i = 0; i < len; i++ )
    {
        r->buf[ ( head + i ) % RING_SIZE ] = data[ i ];
    }

    atomic_store( &r->head, head + ( unsigned ) len );
}

static size_t prvRingGet( ring_t * r, char * out, size_t max )
{
    const unsigned tail = atomic_load( &r->tail );
    size_t n = prvRingUsed( r );

    n = ( n < max ) ? n : max;

    for( size_t i = 0; i < n; i++ )
    {
        out[ i ] = r->buf[ ( tail + i ) % RING_SIZE ];
    }

    atomic_store( &r->tail, tail + ( unsigned ) n );
    return n;
}

static SOCKET prvConnect( void )
{
    struct sockaddr_in addr;
    SOCKET s = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );

    if( s == INVALID_SOCKET )
    {
        return INVALID_SOCKET;
    }

    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_port = htons( ( u_short ) s_port );
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    if( connect( s, ( struct sockaddr * ) &addr, sizeof( addr ) ) != 0 )
    {
        closesocket( s );
        return INVALID_SOCKET;
    }

    return s;
}

static DWORD WINAPI prvLinkThread( LPVOID param )
{
    WSADATA wsa;
    int announced_wait = 0;

    ( void ) param;
    WSAStartup( MAKEWORD( 2, 2 ), &wsa );

    while( s_port > 0 )
    {
        SOCKET s = prvConnect();

        if( s == INVALID_SOCKET )
        {
            if( !announced_wait )
            {
                printf( "[hw] link: Wi-Fi module not reachable on 127.0.0.1:%d, retrying\n", s_port );
                fflush( stdout );
                announced_wait = 1;
            }

            Sleep( 1000 );
            continue;
        }

        announced_wait = 0;
        printf( "[hw] link: connected to Wi-Fi module on 127.0.0.1:%d\n", s_port );
        fflush( stdout );

        /* Fresh session: discard anything stale. */
        atomic_store( &s_tx.tail, atomic_load( &s_tx.head ) );
        atomic_store( &s_connected, 1 );
        vPortGenerateSimulatedInterruptFromWindowsThread( IRQ_LINK_RX );

        for( ; ; )
        {
            fd_set readable;
            struct timeval tv = { 0, 10000 }; /* 10 ms */
            char chunk[ 1024 ];
            int failed = 0;

            /* TX: drain the ring into the socket. */
            while( !failed && ( prvRingUsed( &s_tx ) > 0 ) )
            {
                const size_t n = prvRingGet( &s_tx, chunk, sizeof( chunk ) );

                for( size_t sent = 0; sent < n; )
                {
                    const int r = send( s, chunk + sent, ( int ) ( n - sent ), 0 );

                    if( r <= 0 )
                    {
                        failed = 1;
                        break;
                    }

                    sent += ( size_t ) r;
                }
            }

            /* RX: wait briefly for incoming bytes. */
            FD_ZERO( &readable );
            FD_SET( s, &readable );

            if( !failed && ( select( 0, &readable, NULL, NULL, &tv ) > 0 ) )
            {
                const int r = recv( s, chunk, ( int ) sizeof( chunk ), 0 );

                if( r <= 0 )
                {
                    failed = 1;
                }
                else if( prvRingFree( &s_rx ) >= ( size_t ) r )
                {
                    prvRingPut( &s_rx, chunk, ( size_t ) r );
                    vPortGenerateSimulatedInterruptFromWindowsThread( IRQ_LINK_RX );
                }
                /* else: RX overrun, bytes lost, as on a real UART. */
            }

            if( failed )
            {
                break;
            }
        }

        closesocket( s );
        atomic_store( &s_connected, 0 );
        printf( "[hw] link: Wi-Fi module disconnected\n" );
        fflush( stdout );
        vPortGenerateSimulatedInterruptFromWindowsThread( IRQ_LINK_RX );
    }

    return 0;
}

void hal_link_init( int port )
{
    s_port = port;
    CreateThread( NULL, 0, prvLinkThread, NULL, 0, NULL );
}

size_t hal_link_write( const char * data, size_t len )
{
    if( !atomic_load( &s_connected ) || ( prvRingFree( &s_tx ) < len ) )
    {
        return 0;
    }

    prvRingPut( &s_tx, data, len );
    return len;
}

size_t hal_link_read( char * buf, size_t max )
{
    return prvRingGet( &s_rx, buf, max );
}

int hal_link_connected( void )
{
    return atomic_load( &s_connected );
}
