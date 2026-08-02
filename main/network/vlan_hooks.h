/*
 * lwIP VLAN hooks — included by lwIP via ESP_IDF_LWIP_HOOK_FILENAME.
 * static inline so the compiler eliminates the call entirely.
 */
#include <stdbool.h>
#include "lwip/prot/ethernet.h"

static inline bool lwip_vlan_check(struct netif *netif, struct eth_hdr *eth_hdr, struct eth_vlan_hdr *vlan_hdr)
{
    return true;  /* accept all — EMAC hardware already filtered */
}

static inline int lwip_vlan_set(struct netif *netif, struct pbuf *p,
                                const struct eth_addr *src, const struct eth_addr *dst,
                                u16_t eth_type)
{
    (void)netif; (void)p; (void)src; (void)dst; (void)eth_type;
    return -1;  /* skip software insertion — EMAC hardware does it */
}
