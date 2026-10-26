#pragma once
#include <stdint.h>
#include <stddef.h>

/* CRC-16/CCITT (polynomial 0x1021).
 * Call as: result = crc16(0xFFFF, data, len) ^ 0xFFFF
 * Matches the implementation used by eti-tools and ODR-DabMux for AF packets. */
uint16_t crc16(uint16_t crc, const uint8_t *data, size_t len);
