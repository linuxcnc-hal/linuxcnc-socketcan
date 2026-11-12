#ifndef LSC_HAL_COMPAT_H
#define LSC_HAL_COMPAT_H

/* Compatibility layer for the LinuxCNC HAL API with opaque pin handles
 * (hal_bool_t / hal_sint_t / hal_uint_t / hal_real_t, accessed through
 * hal_get_*() / hal_set_*() and created with hal_pin_new_*()). */
#include <stdbool.h>
#include <stdint.h>

typedef hal_bool_t lsc_bit_t;
typedef hal_uint_t lsc_u32_t;
typedef hal_sint_t lsc_s32_t;
typedef hal_real_t lsc_float_t;

#define lsc_get(h) _Generic((h), \
    hal_bool_t: hal_get_bool, \
    hal_sint_t: hal_get_sint, \
    hal_uint_t: hal_get_uint, \
    hal_real_t: hal_get_real)(h)

#define lsc_set(h, v) _Generic((h), \
    hal_bool_t: hal_set_bool, \
    hal_sint_t: hal_set_sint, \
    hal_uint_t: hal_set_uint, \
    hal_real_t: hal_set_real)((h), (v))

#define lsc_pin_bit_newf(dir, ptr, comp, ...)   hal_pin_new_bool((comp), (dir), (ptr), 0, __VA_ARGS__)
#define lsc_pin_u32_newf(dir, ptr, comp, ...)   hal_pin_new_uint((comp), (dir), (ptr), 0, __VA_ARGS__)
#define lsc_pin_s32_newf(dir, ptr, comp, ...)   hal_pin_new_sint((comp), (dir), (ptr), 0, __VA_ARGS__)
#define lsc_pin_float_newf(dir, ptr, comp, ...) hal_pin_new_real((comp), (dir), (ptr), 0, __VA_ARGS__)

#endif
