#include "sqli_decimal_codec.h"
#include "sqli_base100.h"

#include <string.h>

enum {
    max_precision = 32, floating_scale = 255, limb_digits = 9,
    coefficient_limbs = 4, exponent_bias = 64, positive_flag = 128,
    exponent_min = -64, exponent_max = 63, pair_digits = 2,
    pair_base = 100, max_coefficient_groups = 16
};
static const uint32_t powers10[] = {
    1,10,100,1000,10000,100000,1000000,10000000,100000000
};
struct decimal_wire_type { unsigned precision, scale; size_t width; };

static sqli_status decode_type(uint16_t descriptor, struct decimal_wire_type *out)
{
    unsigned precision = descriptor >> 8, scale = descriptor & 0xff;
    if (precision == 0 || precision > max_precision || (scale != floating_scale && scale > precision))
        return SQLI_PROTO_ERROR;
    *out = (struct decimal_wire_type){precision, scale, (precision + (scale & 1u) + 3) / 2};
    return SQLI_OK;
}

sqli_status sqli_decimal_wire_size(uint16_t descriptor, size_t *out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct decimal_wire_type type;
    sqli_status status = decode_type(descriptor, &type);
    if (status == SQLI_OK)
        *out = type.width;
    return status;
}

static size_t coefficient_digits(const sqli_decimal_parts_t *parts)
{
    size_t digits = (parts->limb_count - 1) * limb_digits;
    uint32_t top = parts->limbs[parts->limb_count - 1];
    do {
        digits++;
        top /= 10;
    } while (top != 0);
    return digits;
}

static size_t trailing_zeros(const sqli_decimal_parts_t *parts)
{
    size_t zeros = 0;
    for (size_t i = 0; i < parts->limb_count; i++) {
        uint32_t limb = parts->limbs[i];
        if (limb == 0) {
            zeros += limb_digits;
            continue;
        }
        while (limb % 10 == 0) {
            zeros++;
            limb /= 10;
        }
        break;
    }
    return zeros;
}

static unsigned coefficient_digit(const sqli_decimal_parts_t *parts, size_t position)
{
    return parts->limbs[position / limb_digits] / powers10[position % limb_digits] % 10;
}

static sqli_status encode_coefficient(const sqli_decimal_parts_t *parts,
                                      const struct decimal_wire_type *type, uint8_t *encoded)
{
    size_t digits = coefficient_digits(parts);
    size_t zeros = trailing_zeros(parts);
    size_t significant = digits - zeros;
    if (type->scale == floating_scale) {
        if (significant > type->precision)
            return SQLI_OUT_OF_RANGE;
    } else {
        int64_t reduction = (int64_t)parts->scale - type->scale;
        if (reduction > (int64_t)zeros)
            return SQLI_INEXACT;
        if ((int64_t)digits - reduction > type->precision)
            return SQLI_OUT_OF_RANGE;
    }
    /* Native resource ceilings bound digits to one million; the wider signed
     * arithmetic also accommodates both int32 scale endpoints. */
    int64_t integral_digits = (int64_t)digits - parts->scale;
    int64_t exponent = integral_digits >= 0 ? (integral_digits + 1) / pair_digits : integral_digits / pair_digits;
    if (exponent < exponent_min || exponent > exponent_max)
        return SQLI_OUT_OF_RANGE;
    size_t padding = (size_t)(exponent * pair_digits - integral_digits);
    size_t groups = (padding + significant + 1) / pair_digits;
    if (groups > max_coefficient_groups || groups > type->width - 1)
        return SQLI_OUT_OF_RANGE;
    for (size_t i = 0; i < significant; i++) {
        size_t position = i + padding;
        unsigned digit = coefficient_digit(parts, digits - 1 - i);
        encoded[1 + position / pair_digits] += (uint8_t)(position % pair_digits == 0 ? digit * 10 : digit);
    }
    encoded[0] = (uint8_t)(positive_flag | (unsigned)(exponent + exponent_bias));
    if (parts->negative) {
        encoded[0] ^= 0xff;
        sqli_base100_radix_complement(encoded + 1, type->width - 1);
    }
    return SQLI_OK;
}

sqli_status sqli_decimal_encode_wire(const sqli_decimal_t *value, uint16_t descriptor,
                                      uint8_t *bytes, size_t capacity, size_t *length)
{
    if (value == NULL || bytes == NULL || length == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct decimal_wire_type type;
    sqli_status status = decode_type(descriptor, &type);
    if (status != SQLI_OK)
        return status;
    sqli_decimal_parts_t parts;
    status = sqli_decimal_get_parts(value, &parts);
    if (status != SQLI_OK)
        return status;
    uint8_t encoded[SQLI_DECIMAL_WIRE_CAPACITY] = {0};
    if (!parts.is_null) {
        if (parts.limb_count == 0)
            encoded[0] = positive_flag;
        else {
            status = encode_coefficient(&parts, &type, encoded);
            if (status != SQLI_OK)
                return status;
        }
    }
    if (capacity < type.width)
        return SQLI_BUFFER_TOO_SMALL;
    memcpy(bytes, encoded, type.width);
    *length = type.width;
    return SQLI_OK;
}

/* A decoded coefficient has at most 34 temporary digits, then must fit the
 * descriptor's 32-digit ceiling. Four limbs cover both stages without a heap. */
static void append_digit(uint32_t *limbs, unsigned digit)
{
    uint64_t carry = digit;
    for (size_t i = 0; i < coefficient_limbs; i++) {
        uint64_t next = (uint64_t)limbs[i] * 10 + carry;
        limbs[i] = (uint32_t)(next % SQLI_DECIMAL_LIMB_BASE);
        carry = next / SQLI_DECIMAL_LIMB_BASE;
    }
}

static sqli_status decode_coefficient(const uint8_t *bytes, const struct decimal_wire_type *type,
                                      sqli_decimal_t *out)
{
    uint8_t groups[SQLI_DECIMAL_WIRE_CAPACITY] = {0};
    size_t count = type->width - 1;
    for (size_t i = 0; i < count; i++) {
        if (bytes[i + 1] >= pair_base)
            return SQLI_PROTO_ERROR;
        groups[i] = bytes[i + 1];
    }
    bool negative = (bytes[0] & positive_flag) == 0;
    if (negative)
        sqli_base100_radix_complement(groups, count);
    while (count != 0 && groups[count - 1] == 0)
        count--;
    if (count == 0) {
        if (bytes[0] != positive_flag)
            return SQLI_PROTO_ERROR;
        return sqli_decimal_set_i64(out, 0, type->scale == floating_scale ? 0 : (int32_t)type->scale);
    }
    if (count > max_coefficient_groups || groups[0] == 0)
        return SQLI_PROTO_ERROR;
    int exponent = negative ? (int)(bytes[0] ^ 0x7f) - exponent_bias :
        (int)(bytes[0] & 0x7f) - exponent_bias;
    bool drop_last_zero = groups[count - 1] % 10 == 0;
    size_t first_digit = groups[0] < 10 ? 1u : 0u;
    size_t end_digit = count * pair_digits - (drop_last_zero ? 1u : 0u);
    size_t digits = end_digit - first_digit;
    int32_t scale = (int32_t)(count * pair_digits) - exponent * pair_digits - (drop_last_zero ? 1 : 0);
    if (digits > type->precision)
        return SQLI_PROTO_ERROR;
    int32_t extra_zeros = 0;
    if (type->scale != floating_scale) {
        extra_zeros = (int32_t)type->scale - scale;
        if (extra_zeros < 0 || (size_t)extra_zeros > type->precision - digits)
            return SQLI_PROTO_ERROR;
        scale = (int32_t)type->scale;
    }
    uint32_t limbs[coefficient_limbs] = {0};
    for (size_t i = first_digit; i < end_digit; i++) {
        unsigned digit = i % pair_digits == 0 ? groups[i / pair_digits] / 10 : groups[i / pair_digits] % 10;
        append_digit(limbs, digit);
    }
    for (int32_t i = 0; i < extra_zeros; i++)
        append_digit(limbs, 0);
    sqli_decimal_parts_t parts = {
        .limbs = limbs, .limb_count = coefficient_limbs, .scale = scale, .negative = negative
    };
    return sqli_decimal_set_parts(out, &parts);
}

sqli_status sqli_decimal_decode_wire(const uint8_t *bytes, size_t length,
                                      uint16_t descriptor, sqli_decimal_t *out)
{
    if (bytes == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct decimal_wire_type type;
    sqli_status status = decode_type(descriptor, &type);
    if (status != SQLI_OK)
        return status;
    if (length != type.width)
        return SQLI_PROTO_ERROR;
    bool is_null = true;
    for (size_t i = 0; i < length; i++)
        is_null = is_null && bytes[i] == 0;
    if (is_null)
        return sqli_decimal_set_null(out);
    return decode_coefficient(bytes, &type, out);
}
