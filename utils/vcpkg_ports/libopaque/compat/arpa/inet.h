/* Windows stand-in for the one <arpa/inet.h> function liboprf's toprf.c uses. Windows targets are little-endian. */
#ifndef LIBOPAQUE_COMPAT_ARPA_INET_H
#define LIBOPAQUE_COMPAT_ARPA_INET_H

#include <stdlib.h>

#ifndef htons
#define htons(x) _byteswap_ushort(x)
#endif

#endif
