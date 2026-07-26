#ifndef NETWORK_EVENTS_H
#define NETWORK_EVENTS_H

#include <stdint.h>

/* Event group bits for Ethernet state, shared across layers */
#define EVENT_ETH_LINK_UP    (1U << 3)
#define EVENT_ETH_HAS_IP     (1U << 4)
#define EVENT_ETH_LINK_DOWN  (1U << 5)

/** Set by web server when certs or config change -> barrier reconnects */
#define EVENT_RECONNECT        (1U << 6)

#endif
