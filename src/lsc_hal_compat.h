#ifndef LSC_HAL_COMPAT_H
#define LSC_HAL_COMPAT_H

/* Compatibility layer: newer LinuxCNC removed hal_bit_t/hal_u32_t/hal_s32_t
 * and the hal_pin_<type>_newf helpers. hal_pin_newf() exists in all versions. */
#include <stdbool.h>
#include <stdint.h>

typedef bool lsc_bit_t;
typedef uint32_t lsc_u32_t;
typedef int32_t lsc_s32_t;
typedef double lsc_float_t;

#define lsc_pin_bit_newf(dir, ptr, comp, ...)   hal_pin_newf(HAL_BIT, (dir), (void **)(ptr), (comp), __VA_ARGS__)
#define lsc_pin_u32_newf(dir, ptr, comp, ...)   hal_pin_newf(HAL_U32, (dir), (void **)(ptr), (comp), __VA_ARGS__)
#define lsc_pin_s32_newf(dir, ptr, comp, ...)   hal_pin_newf(HAL_S32, (dir), (void **)(ptr), (comp), __VA_ARGS__)
#define lsc_pin_float_newf(dir, ptr, comp, ...) hal_pin_newf(HAL_FLOAT, (dir), (void **)(ptr), (comp), __VA_ARGS__)

#endif
