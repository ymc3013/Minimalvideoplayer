
#include <stdint.h>

/*
 * Compatibility implementation for FFmpeg's network.o.
 * Nintendo 3DS ARM is little-endian.
 */
uint32_t ntohl(uint32_t netlong)
{
    return __builtin_bswap32(netlong);
}
