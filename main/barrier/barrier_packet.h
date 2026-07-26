#ifndef BARRIER_PACKET_H
#define BARRIER_PACKET_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    PACKET_QINF,
    PACKET_DINF,
    PACKET_CIAK,
    PACKET_CALV,
    PACKET_CNOP,
    PACKET_CINN,
    PACKET_COUT,
    PACKET_DMMV,
    PACKET_DMRM,
    PACKET_DMUP,
    PACKET_DMDN,
    PACKET_DMWM,
    PACKET_DKUP,
    PACKET_DKDN,
    PACKET_DKRP,
    PACKET_CCLP,
    PACKET_DCLP,
    PACKET_CBYE,
    PACKET_EUNK,
    PACKET_EBSY,
    PACKET_EBAD,
    PACKET_EICV,
    PACKET_CROP,
    PACKET_DSOP,
    PACKET_UNKNOWN
} packet_type_t;

typedef struct {
    packet_type_t type;
    union {
        struct { uint16_t x, y; }                          mouse_move_abs;
        struct { int16_t dx, dy; }                         mouse_move_rel;
        struct { int8_t id; }                              mouse_up;
        struct { int8_t id; }                              mouse_down;
        struct { int16_t dx, dy; }                         mouse_wheel;
        struct { uint16_t x, y; uint32_t seq; uint16_t mask; } cursor_enter;
        struct { uint16_t id, mask, button; }             key_up;
        struct { uint16_t id, mask, button; }             key_down;
        struct { uint16_t id, mask, count, button; }     key_repeat;
        struct { uint16_t x, y, w, h, dummy, mx, my; }   device_info;
        struct { uint8_t id; uint32_t seq; }              clipboard;
        struct { uint16_t major, minor; }                 incompatible;
    };
} packet_t;

esp_err_t packet_parse(uint8_t *buf, size_t len, packet_t *out);
esp_err_t packet_serialize(packet_t *pkt, uint8_t *buf, size_t *out_len);
const char *packet_type_to_string(packet_type_t type);

#endif
