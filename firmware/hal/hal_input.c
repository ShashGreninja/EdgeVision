#include <winsock2.h>
#include <windows.h>
#include <conio.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

#include "hal/hal_input.h"
#include "hal/hal_irq.h"

/* Everything in this file runs in plain Windows threads ("hardware"), so it
 * uses printf directly and talks to the firmware only by raising interrupts. */

static hal_input_cfg_t s_cfg;

static void prvRaiseMotion( const char * source )
{
    printf( "[hw] motion pin raised (%s)\n", source );
    fflush( stdout );
    vPortGenerateSimulatedInterruptFromWindowsThread( IRQ_MOTION );
}

static DWORD WINAPI prvKeyboardThread( LPVOID param )
{
    ( void ) param;

    for( ; ; )
    {
        const int c = _getch();

        if( ( c == 'm' ) || ( c == 'M' ) )
        {
            prvRaiseMotion( "keyboard" );
        }
        else if( ( c == 'q' ) || ( c == 'Q' ) )
        {
            printf( "[hw] quit requested\n" );
            fflush( stdout );
            ExitProcess( 0 );
        }
    }
}

static DWORD WINAPI prvUdpThread( LPVOID param )
{
    WSADATA wsa;
    SOCKET sock;
    struct sockaddr_in addr;
    char buf[ 256 ];

    ( void ) param;

    if( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 )
    {
        printf( "[hw] udp: WSAStartup failed\n" );
        return 1;
    }

    sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_port = htons( ( u_short ) s_cfg.udp_port );
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    if( ( sock == INVALID_SOCKET ) || ( bind( sock, ( struct sockaddr * ) &addr, sizeof( addr ) ) != 0 ) )
    {
        printf( "[hw] udp: cannot bind 127.0.0.1:%d\n", s_cfg.udp_port );
        return 1;
    }

    printf( "[hw] udp: listening for MOTION on 127.0.0.1:%d\n", s_cfg.udp_port );

    for( ; ; )
    {
        const int n = recvfrom( sock, buf, ( int ) sizeof( buf ) - 1, 0, NULL, NULL );

        if( n <= 0 )
        {
            continue;
        }

        buf[ n ] = '\0';

        if( strncmp( buf, "MOTION", 6 ) == 0 )
        {
            /* "MOTION ring human <device>" from the gateway, or plain "MOTION". */
            prvRaiseMotion( ( n > 7 ) ? buf + 7 : "udp" );
        }
    }
}

static DWORD WINAPI prvAutoMotionThread( LPVOID param )
{
    ( void ) param;
    Sleep( 1000 );

    while( s_cfg.auto_motion_s > 0 )
    {
        prvRaiseMotion( "auto" );
        Sleep( ( DWORD ) s_cfg.auto_motion_s * 1000 );
    }

    return 0;
}

void hal_input_init( const hal_input_cfg_t * cfg )
{
    s_cfg = *cfg;

    if( s_cfg.keyboard && _isatty( _fileno( stdin ) ) )
    {
        CreateThread( NULL, 0, prvKeyboardThread, NULL, 0, NULL );
    }

    if( s_cfg.udp_port > 0 )
    {
        CreateThread( NULL, 0, prvUdpThread, NULL, 0, NULL );
    }

    if( s_cfg.auto_motion_s > 0 )
    {
        CreateThread( NULL, 0, prvAutoMotionThread, NULL, 0, NULL );
    }
}
