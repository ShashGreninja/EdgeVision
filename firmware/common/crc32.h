#ifndef EV_CRC32_H
#define EV_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-32 (IEEE 802.3), nibble-table version: a 64-byte table instead of 1 KB. */
uint32_t crc32_compute( const uint8_t * data, size_t len );

#ifdef __cplusplus
}
#endif

#endif /* EV_CRC32_H */
