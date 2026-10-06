#ifndef NETWORK_H
#define NETWORK_H

/* NetworkTask: delivers events to the cloud through the Wi-Fi module.
 *
 * Events are kept in a fixed store in RAM until the module confirms the
 * MQTT publish (store-and-forward). While the network is down, detection
 * carries on and events wait; when it comes back they are sent in order.
 * If the store fills up, the oldest event is dropped.
 *
 * Module protocol (one line each, AT-command style):
 *   firmware -> module   AT+PUB=<id>,<topic>,<json>     publish, expect +PUBACK
 *                        AT+TEL=<json>                  telemetry, no reply
 *                        AT+NETDOWN=<seconds>           simulate a network outage
 *   module -> firmware   +LINK:UP / +LINK:DOWN          cloud connection state
 *                        +PUBACK:<id>,OK|ERR            result of AT+PUB */

#include <stdint.h>

#include "pipeline/pipeline_types.h"

#define NET_STORE_LEN        16   /* Events held while offline.        */
#define NET_JSON_MAX         320  /* Longest event JSON.               */
#define NET_ACK_TIMEOUT_MS   3000 /* Resend if no +PUBACK by then.     */
#define NET_RETRY_MS         1000 /* Wait after a failed publish.      */

typedef struct
{
    uint8_t cable;          /* Link to the module is connected.        */
    uint8_t online;         /* Module reports the cloud is reachable.  */
    uint32_t queued;        /* Events waiting in the store now.        */
    uint32_t published;     /* Events confirmed by the module.         */
    uint32_t dropped;       /* Events lost because the store was full. */
    uint32_t retries;       /* Publishes that had to be repeated.      */
    uint32_t bytes_up;      /* Payload + topic bytes published.        */
    uint32_t telemetry_sent;
} network_stats_t;

int network_init( const char * device_id, int link_port );

/* Hand an event to NetworkTask. Never blocks. */
void network_post_event( const event_t * ev );

/* Send one telemetry line to the module if the cable is connected. */
void network_send_telemetry( const char * json );

/* Ask the module to simulate a network outage. */
void network_simulate_outage( uint32_t seconds );

void network_get_stats( network_stats_t * out );

const char * network_device_id( void );

#endif /* NETWORK_H */
