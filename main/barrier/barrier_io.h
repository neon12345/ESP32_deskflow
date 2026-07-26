#ifndef BARRIER_IO_H
#define BARRIER_IO_H

#include <stdint.h>

uint32_t io_read_u32_be(const uint8_t *buf);
uint16_t io_read_u16_be(const uint8_t *buf);
int16_t  io_read_i16_be(const uint8_t *buf);
int8_t   io_read_i8(const uint8_t *buf);
void io_write_u32_be(uint8_t *buf, uint32_t val);
void io_write_u16_be(uint8_t *buf, uint16_t val);
void io_write_i16_be(uint8_t *buf, int16_t val);

#endif
