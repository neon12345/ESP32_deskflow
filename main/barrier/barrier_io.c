#include "barrier_io.h"

uint32_t io_read_u32_be(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |
           ((uint32_t)buf[3]);
}

uint16_t io_read_u16_be(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) |
           ((uint16_t)buf[1]);
}

int16_t io_read_i16_be(const uint8_t *buf)
{
    return (int16_t)io_read_u16_be(buf);
}

int8_t  io_read_i8(const uint8_t *buf)
{
    return (int8_t)buf[0];
}

void io_write_u32_be(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)((val >> 24) & 0xFF);
    buf[1] = (uint8_t)((val >> 16) & 0xFF);
    buf[2] = (uint8_t)((val >> 8)  & 0xFF);
    buf[3] = (uint8_t)(val & 0xFF);
}

void io_write_u16_be(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)((val >> 8) & 0xFF);
    buf[1] = (uint8_t)(val & 0xFF);
}

void io_write_i16_be(uint8_t *buf, int16_t val)
{
    io_write_u16_be(buf, (uint16_t)val);
}
