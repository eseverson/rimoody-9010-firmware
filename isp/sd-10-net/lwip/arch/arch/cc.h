/* lwIP compiler/platform glue for the NDS32 payload and the host tests. */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H
#include <stdint.h>
#include <stddef.h>
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_INTTYPES_H 1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"
#define LWIP_RAND() ((u32_t)lwip_rand())
unsigned int lwip_rand(void);
void lwip_assert(const char *msg, const char *file, int line);
#define LWIP_PLATFORM_ASSERT(x) lwip_assert((x), __FILE__, __LINE__)
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#endif
