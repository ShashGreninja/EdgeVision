#ifndef HAL_LINK_H
#define HAL_LINK_H

/* Serial link to the Wi-Fi module (the simulator's UART).
 *
 * Low-power cameras often pair the main chip with a Wi-Fi module that runs
 * the TCP/TLS/MQTT stack itself and is driven by short text commands, so the
 * firmware never needs a TLS stack in its own RAM. Here the module is a
 * separate process (netmodule/wifi_module.py) and the "UART" is a localhost
 * TCP connection, serviced by a Windows thread with a TX and an RX ring
 * buffer, like a UART with DMA. IRQ_LINK_RX fires when bytes arrive or the
 * cable state changes. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the link thread, connecting to the module on 127.0.0.1:port. */
void hal_link_init( int port );

/* Queue bytes for transmission. All or nothing: returns len, or 0 if the TX
 * ring has no room (or the cable is unplugged). Safe from tasks; callers must
 * serialise among themselves. */
size_t hal_link_write( const char * data, size_t len );

/* Take up to max received bytes. Returns the number copied. */
size_t hal_link_read( char * buf, size_t max );

/* 1 while the cable to the module is connected. */
int hal_link_connected( void );

#ifdef __cplusplus
}
#endif

#endif /* HAL_LINK_H */
