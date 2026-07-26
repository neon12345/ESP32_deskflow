#include "barrier_packet.h"
#include "barrier_io.h"
#include <string.h>
#include <stdlib.h>

#define BARRIER_CODE_LEN 4
#define BARRIER_HEADER_LEN (4 + BARRIER_CODE_LEN) /* u32 length + 4-byte ASCII code */

static packet_type_t code_to_type(const char *code)
{
    if (memcmp(code, "QINF", 4) == 0) return PACKET_QINF;
    if (memcmp(code, "DINF", 4) == 0) return PACKET_DINF;
    if (memcmp(code, "CIAK", 4) == 0) return PACKET_CIAK;
    if (memcmp(code, "CALV", 4) == 0) return PACKET_CALV;
    if (memcmp(code, "CNOP", 4) == 0) return PACKET_CNOP;
    if (memcmp(code, "CINN", 4) == 0) return PACKET_CINN;
    if (memcmp(code, "COUT", 4) == 0) return PACKET_COUT;
    if (memcmp(code, "DMMV", 4) == 0) return PACKET_DMMV;
    if (memcmp(code, "DMRM", 4) == 0) return PACKET_DMRM;
    if (memcmp(code, "DMUP", 4) == 0) return PACKET_DMUP;
    if (memcmp(code, "DMDN", 4) == 0) return PACKET_DMDN;
    if (memcmp(code, "DMWM", 4) == 0) return PACKET_DMWM;
    if (memcmp(code, "DKUP", 4) == 0) return PACKET_DKUP;
    if (memcmp(code, "DKDN", 4) == 0) return PACKET_DKDN;
    if (memcmp(code, "DKRP", 4) == 0) return PACKET_DKRP;
    if (memcmp(code, "CCLP", 4) == 0) return PACKET_CCLP;
    if (memcmp(code, "DCLP", 4) == 0) return PACKET_DCLP;
    if (memcmp(code, "CBYE", 4) == 0) return PACKET_CBYE;
    if (memcmp(code, "EUNK", 4) == 0) return PACKET_EUNK;
    if (memcmp(code, "EBSY", 4) == 0) return PACKET_EBSY;
    if (memcmp(code, "EBAD", 4) == 0) return PACKET_EBAD;
    if (memcmp(code, "EICV", 4) == 0) return PACKET_EICV;
    if (memcmp(code, "CROP", 4) == 0) return PACKET_CROP;
    if (memcmp(code, "DSOP", 4) == 0) return PACKET_DSOP;
    return PACKET_UNKNOWN;
}

static const char *type_to_code(packet_type_t type)
{
    switch (type) {
        case PACKET_QINF:  return "QINF";
        case PACKET_DINF:  return "DINF";
        case PACKET_CIAK:  return "CIAK";
        case PACKET_CALV:  return "CALV";
        case PACKET_CNOP:  return "CNOP";
        case PACKET_CINN:  return "CINN";
        case PACKET_COUT:  return "COUT";
        case PACKET_DMMV:  return "DMMV";
        case PACKET_DMRM:  return "DMRM";
        case PACKET_DMUP:  return "DMUP";
        case PACKET_DMDN:  return "DMDN";
        case PACKET_DMWM:  return "DMWM";
        case PACKET_DKUP:  return "DKUP";
        case PACKET_DKDN:  return "DKDN";
        case PACKET_DKRP:  return "DKRP";
        case PACKET_CCLP:  return "CCLP";
        case PACKET_DCLP:  return "DCLP";
        case PACKET_CBYE:  return "CBYE";
        case PACKET_EUNK:  return "EUNK";
        case PACKET_EBSY:  return "EBSY";
        case PACKET_EBAD:  return "EBAD";
        case PACKET_EICV:  return "EICV";
        case PACKET_CROP:  return "CROP";
        case PACKET_DSOP:  return "DSOP";
        default:           return "????";
    }
}

esp_err_t packet_parse(uint8_t *buf, size_t len, packet_t *out)
{
    if (len < BARRIER_HEADER_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Payload length is the first 4 bytes (big-endian) */
    uint32_t payload_len = io_read_u32_be(buf);

    /* The 4-byte ASCII code starts at offset 4 */
    const uint8_t *code_ptr = &buf[4];
    char code[5] = { 0 };
    memcpy(code, code_ptr, BARRIER_CODE_LEN);

    packet_type_t type = code_to_type(code);

    /* Payload data starts at offset 8 */
    const uint8_t *data = &buf[8];
    size_t data_len = (payload_len >= BARRIER_CODE_LEN) ? (payload_len - BARRIER_CODE_LEN) : 0;

    memset(out, 0, sizeof(packet_t));
    out->type = type;

    switch (type) {
        case PACKET_QINF:
        case PACKET_CIAK:
        case PACKET_CALV:
        case PACKET_CNOP:
        case PACKET_COUT:
        case PACKET_CBYE:
        case PACKET_EUNK:
        case PACKET_EBSY:
        case PACKET_EBAD:
        case PACKET_CROP:
            /* No payload data */
            break;

        case PACKET_DINF:
            if (data_len < 14) return ESP_ERR_INVALID_SIZE;
            out->device_info.x     = io_read_u16_be(&data[0]);
            out->device_info.y     = io_read_u16_be(&data[2]);
            out->device_info.w     = io_read_u16_be(&data[4]);
            out->device_info.h     = io_read_u16_be(&data[6]);
            out->device_info.dummy = io_read_u16_be(&data[8]);
            out->device_info.mx    = io_read_u16_be(&data[10]);
            out->device_info.my    = io_read_u16_be(&data[12]);
            break;

        case PACKET_DMMV:
            if (data_len < 4) return ESP_ERR_INVALID_SIZE;
            out->mouse_move_abs.x = io_read_u16_be(&data[0]);
            out->mouse_move_abs.y = io_read_u16_be(&data[2]);
            break;

        case PACKET_DMRM:
            if (data_len < 4) return ESP_ERR_INVALID_SIZE;
            out->mouse_move_rel.dx = io_read_i16_be(&data[0]);
            out->mouse_move_rel.dy = io_read_i16_be(&data[2]);
            break;

        case PACKET_DMUP:
            if (data_len < 1) return ESP_ERR_INVALID_SIZE;
            out->mouse_up.id = io_read_i8(&data[0]);
            break;

        case PACKET_DMDN:
            if (data_len < 1) return ESP_ERR_INVALID_SIZE;
            out->mouse_down.id = io_read_i8(&data[0]);
            break;

        case PACKET_DMWM:
            if (data_len < 4) return ESP_ERR_INVALID_SIZE;
            out->mouse_wheel.dx = io_read_i16_be(&data[0]);
            out->mouse_wheel.dy = io_read_i16_be(&data[2]);
            break;

        case PACKET_CINN:
            if (data_len < 10) return ESP_ERR_INVALID_SIZE;
            out->cursor_enter.x    = io_read_u16_be(&data[0]);
            out->cursor_enter.y    = io_read_u16_be(&data[2]);
            out->cursor_enter.seq  = io_read_u32_be(&data[4]);
            out->cursor_enter.mask = io_read_u16_be(&data[8]);
            break;

        case PACKET_DKUP:
            if (data_len < 6) return ESP_ERR_INVALID_SIZE;
            out->key_up.id    = io_read_u16_be(&data[0]);
            out->key_up.mask  = io_read_u16_be(&data[2]);
            out->key_up.button = io_read_u16_be(&data[4]);
            break;

        case PACKET_DKDN:
            if (data_len < 6) return ESP_ERR_INVALID_SIZE;
            out->key_down.id    = io_read_u16_be(&data[0]);
            out->key_down.mask  = io_read_u16_be(&data[2]);
            out->key_down.button = io_read_u16_be(&data[4]);
            break;

        case PACKET_DKRP:
            if (data_len < 8) return ESP_ERR_INVALID_SIZE;
            out->key_repeat.id    = io_read_u16_be(&data[0]);
            out->key_repeat.mask  = io_read_u16_be(&data[2]);
            out->key_repeat.count = io_read_u16_be(&data[4]);
            out->key_repeat.button = io_read_u16_be(&data[6]);
            break;

        case PACKET_CCLP:
            /* 5-byte payload (u8 id + u32 seq) is validated here.  The actual
             * clipboard data is transferred out-of-band, so we only record
             * that the packet was well-formed. */
            if (data_len < 5) return ESP_ERR_INVALID_SIZE;
            break;

        case PACKET_DCLP:
            if (data_len < 5) return ESP_ERR_INVALID_SIZE;
            out->clipboard.id  = data[0];
            out->clipboard.seq = io_read_u32_be(&data[1]);
            break;

        case PACKET_EICV:
            if (data_len < 4) return ESP_ERR_INVALID_SIZE;
            out->incompatible.major = io_read_u16_be(&data[0]);
            out->incompatible.minor = io_read_u16_be(&data[2]);
            break;

        default:
            break;
    }

    return ESP_OK;
}

esp_err_t packet_serialize(packet_t *pkt, uint8_t *buf, size_t *out_len)
{
    if (pkt == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *code = type_to_code(pkt->type);
    size_t payload_data_len = 0;

    switch (pkt->type) {
        case PACKET_QINF:
        case PACKET_CIAK:
        case PACKET_CALV:
        case PACKET_CNOP:
        case PACKET_COUT:
        case PACKET_CBYE:
        case PACKET_EUNK:
        case PACKET_EBSY:
        case PACKET_EBAD:
        case PACKET_CROP:
            payload_data_len = 0;
            break;

        case PACKET_DINF:
            payload_data_len = 14; /* 7 x u16 */
            break;

        case PACKET_DMMV:
            payload_data_len = 4; /* 2 x u16 */
            break;

        case PACKET_DMRM:
        case PACKET_DMWM:
            payload_data_len = 4; /* 2 x i16 */
            break;

        case PACKET_DMUP:
        case PACKET_DMDN:
            payload_data_len = 1; /* i8 */
            break;

        case PACKET_CINN:
            payload_data_len = 12; /* u16, u16, u32, u16 */
            break;

        case PACKET_DKUP:
        case PACKET_DKDN:
            payload_data_len = 6; /* 3 x u16 */
            break;

        case PACKET_DKRP:
            payload_data_len = 8; /* 4 x u16 */
            break;

        case PACKET_CCLP:
        case PACKET_DCLP:
            payload_data_len = 5; /* u8 + u32 */
            break;

        case PACKET_EICV:
            payload_data_len = 4; /* 2 x u16 */
            break;

        case PACKET_DSOP:
            /* DSOP (display snapshot) payload length is variable and not
             * fully specified in the Barrier protocol.  Serialization
             * currently emits a zero-length payload.  Remove or implement
             * when a concrete spec is available. */
            payload_data_len = 0;
            break;

        default:
            payload_data_len = 0;
            break;
    }

    /* Bounds check: ensure output buffer can hold header + payload */
    if (BARRIER_HEADER_LEN + payload_data_len > *out_len) {
        return ESP_ERR_NO_MEM;
    }

    /* Total payload = 4-byte code + data */
    uint32_t total_payload = (uint32_t)(BARRIER_CODE_LEN + payload_data_len);

    /* Write 4-byte length prefix */
    io_write_u32_be(&buf[0], total_payload);

    /* Write 4-byte ASCII code */
    memcpy(&buf[4], code, BARRIER_CODE_LEN);

    /* Write payload data */
    uint8_t *data = &buf[8];
    switch (pkt->type) {
        case PACKET_DINF:
            io_write_u16_be(&data[0],  pkt->device_info.x);
            io_write_u16_be(&data[2],  pkt->device_info.y);
            io_write_u16_be(&data[4],  pkt->device_info.w);
            io_write_u16_be(&data[6],  pkt->device_info.h);
            io_write_u16_be(&data[8],  pkt->device_info.dummy);
            io_write_u16_be(&data[10], pkt->device_info.mx);
            io_write_u16_be(&data[12], pkt->device_info.my);
            break;

        case PACKET_DMMV:
            io_write_u16_be(&data[0], pkt->mouse_move_abs.x);
            io_write_u16_be(&data[2], pkt->mouse_move_abs.y);
            break;

        case PACKET_DMRM:
            io_write_i16_be(&data[0], pkt->mouse_move_rel.dx);
            io_write_i16_be(&data[2], pkt->mouse_move_rel.dy);
            break;

        case PACKET_DMUP:
            data[0] = (uint8_t)pkt->mouse_up.id;
            break;

        case PACKET_DMDN:
            data[0] = (uint8_t)pkt->mouse_down.id;
            break;

        case PACKET_DMWM:
            io_write_i16_be(&data[0], pkt->mouse_wheel.dx);
            io_write_i16_be(&data[2], pkt->mouse_wheel.dy);
            break;

        case PACKET_CINN:
            io_write_u16_be(&data[0],  pkt->cursor_enter.x);
            io_write_u16_be(&data[2],  pkt->cursor_enter.y);
            io_write_u32_be(&data[4],  pkt->cursor_enter.seq);
            io_write_u16_be(&data[8],  pkt->cursor_enter.mask);
            break;

        case PACKET_DKUP:
            io_write_u16_be(&data[0], pkt->key_up.id);
            io_write_u16_be(&data[2], pkt->key_up.mask);
            io_write_u16_be(&data[4], pkt->key_up.button);
            break;

        case PACKET_DKDN:
            io_write_u16_be(&data[0], pkt->key_down.id);
            io_write_u16_be(&data[2], pkt->key_down.mask);
            io_write_u16_be(&data[4], pkt->key_down.button);
            break;

        case PACKET_DKRP:
            io_write_u16_be(&data[0], pkt->key_repeat.id);
            io_write_u16_be(&data[2], pkt->key_repeat.mask);
            io_write_u16_be(&data[4], pkt->key_repeat.count);
            io_write_u16_be(&data[6], pkt->key_repeat.button);
            break;

        case PACKET_CCLP:
        case PACKET_DCLP:
            data[0] = pkt->clipboard.id;
            io_write_u32_be(&data[1], pkt->clipboard.seq);
            break;

        case PACKET_EICV:
            io_write_u16_be(&data[0], pkt->incompatible.major);
            io_write_u16_be(&data[2], pkt->incompatible.minor);
            break;

        default:
            break;
    }

    *out_len = BARRIER_HEADER_LEN + payload_data_len;
    return ESP_OK;
}

const char *packet_type_to_string(packet_type_t type)
{
    return type_to_code(type);
}
