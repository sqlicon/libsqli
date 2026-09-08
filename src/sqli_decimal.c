#include "libsqli/sqli_decimal.h"
#include "libsqli/sqli.h"

#include <stdlib.h>
#include <string.h>

enum {
    decimal_limb_digits = 9,
    decimal_inline_limbs = 4,
    decimal_max_limbs = (SQLI_DECIMAL_MAX_DIGITS + decimal_limb_digits - 1) /
                        decimal_limb_digits,
    decimal_plain_min_exponent = -6,
    decimal_exponent_capacity = 24
};

static const uint32_t powers_of_ten[] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000
};

struct sqli_decimal {
    uint32_t *limbs;
    size_t used;
    size_t capacity;
    int32_t scale;
    bool negative;
    bool is_null;
    uint32_t inline_limbs[decimal_inline_limbs];
};

static void decimal_init(sqli_decimal_t *value)
{
    memset(value, 0, sizeof(*value));
    value->limbs = value->inline_limbs;
    value->capacity = decimal_inline_limbs;
    value->is_null = true;
}

static void decimal_clear(sqli_decimal_t *value)
{
    if (value->limbs != value->inline_limbs)
        free(value->limbs);
}

static sqli_status decimal_reserve(sqli_decimal_t *value, size_t count)
{
    if (count > decimal_max_limbs || count > SIZE_MAX / sizeof(*value->limbs))
        return SQLI_LIMIT_EXCEEDED;
    if (count <= value->capacity)
        return SQLI_OK;
    uint32_t *limbs = malloc(count * sizeof(*limbs));
    if (limbs == NULL)
        return SQLI_ALLOC_FAIL;
    if (value->used != 0)
        memcpy(limbs, value->limbs, value->used * sizeof(*limbs));
    decimal_clear(value);
    value->limbs = limbs;
    value->capacity = count;
    return SQLI_OK;
}

/* Transfer a fully validated temporary. Inline pointers never escape their
 * owning object. No allocation can fail after the destination is replaced. */
static void decimal_move(sqli_decimal_t *destination, sqli_decimal_t *source)
{
    decimal_clear(destination);
    destination->used = source->used;
    destination->scale = source->scale;
    destination->negative = source->negative;
    destination->is_null = source->is_null;
    if (source->limbs == source->inline_limbs) {
        destination->limbs = destination->inline_limbs;
        destination->capacity = decimal_inline_limbs;
        memcpy(destination->inline_limbs, source->inline_limbs,
               sizeof(destination->inline_limbs));
    } else {
        destination->limbs = source->limbs;
        destination->capacity = source->capacity;
        source->limbs = source->inline_limbs;
    }
}

static void decimal_trim(sqli_decimal_t *value)
{
    while (value->used != 0 && value->limbs[value->used - 1] == 0)
        value->used--;
    if (value->used == 0)
        value->negative = false;
}

static size_t decimal_digits(const sqli_decimal_t *value)
{
    if (value->used == 0)
        return 1;
    size_t digits = (value->used - 1) * decimal_limb_digits;
    uint32_t top = value->limbs[value->used - 1];
    do {
        digits++;
        top /= 10;
    } while (top != 0);
    return digits;
}

/* Read one decimal digit from most to least significant. Positions beyond the
 * coefficient are virtual zeros, permitting comparison without huge rescaling. */
static unsigned decimal_digit(const sqli_decimal_t *value, size_t digits, size_t position)
{
    if (value->used == 0 || position >= digits)
        return 0;
    size_t offset = digits - position - 1;
    return (value->limbs[offset / decimal_limb_digits] /
            powers_of_ten[offset % decimal_limb_digits]) % 10;
}

sqli_status sqli_decimal_create(sqli_decimal_t **out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_decimal_t *value = malloc(sizeof(*value));
    if (value == NULL)
        return SQLI_ALLOC_FAIL;
    decimal_init(value);
    *out = value;
    return SQLI_OK;
}

void sqli_decimal_destroy(sqli_decimal_t *value)
{
    if (value == NULL)
        return;
    decimal_clear(value);
    free(value);
}

sqli_status sqli_decimal_is_null(const sqli_decimal_t *value, bool *out)
{
    if (value == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = value->is_null;
    return SQLI_OK;
}

sqli_status sqli_decimal_set_null(sqli_decimal_t *value)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    value->is_null = true;
    value->used = 0;
    value->scale = 0;
    value->negative = false;
    return SQLI_OK;
}

sqli_status sqli_decimal_set_parts(sqli_decimal_t *value, const sqli_decimal_parts_t *parts)
{
    if (value == NULL || parts == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (parts->is_null)
        return sqli_decimal_set_null(value);
    if (parts->limb_count > decimal_max_limbs)
        return SQLI_LIMIT_EXCEEDED;
    if (parts->limb_count != 0 && parts->limbs == NULL)
        return SQLI_INVALID_ARGUMENT;
    for (size_t i = 0; i < parts->limb_count; i++) {
        if (parts->limbs[i] >= SQLI_DECIMAL_LIMB_BASE)
            return SQLI_INVALID_ARGUMENT;
    }
    size_t count = parts->limb_count;
    while (count != 0 && parts->limbs[count - 1] == 0)
        count--;
    sqli_decimal_t temporary;
    decimal_init(&temporary);
    sqli_status status = decimal_reserve(&temporary, count);
    if (status != SQLI_OK)
        goto cleanup;
    if (count != 0)
        memcpy(temporary.limbs, parts->limbs, count * sizeof(*temporary.limbs));
    temporary.used = count;
    temporary.scale = parts->scale;
    temporary.negative = count != 0 && parts->negative;
    temporary.is_null = false;
    if (decimal_digits(&temporary) > SQLI_DECIMAL_MAX_DIGITS) {
        status = SQLI_LIMIT_EXCEEDED;
        goto cleanup;
    }
    decimal_move(value, &temporary);
cleanup:
    decimal_clear(&temporary);
    return status;
}

sqli_status sqli_decimal_get_parts(const sqli_decimal_t *value, sqli_decimal_parts_t *out)
{
    if (value == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_decimal_parts_t parts = {
        .limbs = value->used != 0 ? value->limbs : NULL,
        .limb_count = value->used,
        .scale = value->scale,
        .negative = value->negative,
        .is_null = value->is_null
    };
    *out = parts;
    return SQLI_OK;
}

sqli_status sqli_decimal_copy(sqli_decimal_t *destination, const sqli_decimal_t *source)
{
    if (destination == NULL || source == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (destination == source)
        return SQLI_OK;
    sqli_decimal_parts_t parts;
    sqli_status status = sqli_decimal_get_parts(source, &parts);
    if (status != SQLI_OK)
        return status;
    return sqli_decimal_set_parts(destination, &parts);
}

sqli_status sqli_decimal_set_i64(sqli_decimal_t *value, int64_t coefficient, int32_t scale)
{
    uint64_t magnitude = coefficient < 0 ? (uint64_t)(-(coefficient + 1)) + 1u :
                                         (uint64_t)coefficient;
    uint32_t limbs[decimal_inline_limbs];
    size_t used = 0;
    while (magnitude != 0) {
        limbs[used++] = (uint32_t)(magnitude % SQLI_DECIMAL_LIMB_BASE);
        magnitude /= SQLI_DECIMAL_LIMB_BASE;
    }
    sqli_decimal_parts_t parts = {
        .limbs = limbs, .limb_count = used, .scale = scale,
        .negative = coefficient < 0, .is_null = false
    };
    return sqli_decimal_set_parts(value, &parts);
}

sqli_status sqli_decimal_to_i64(const sqli_decimal_t *value, int64_t *out)
{
    if (value == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (value->is_null)
        return SQLI_NULL_VALUE;
    if (value->used == 0) {
        *out = 0;
        return SQLI_OK;
    }
    size_t digits = decimal_digits(value);
    int64_t whole_digits = (int64_t)digits - value->scale;
    if (whole_digits <= 0)
        return SQLI_INEXACT;
    /* Reject fractional values before checking the integral range. */
    if (value->scale > 0) {
        for (size_t i = (size_t)whole_digits; i < digits; i++) {
            if (decimal_digit(value, digits, i) != 0)
                return SQLI_INEXACT;
        }
    }
    if (whole_digits > 19) /* Maximum decimal width of an int64 magnitude. */
        return SQLI_OUT_OF_RANGE;
    uint64_t limit = (uint64_t)INT64_MAX + (value->negative ? 1u : 0u);
    uint64_t magnitude = 0;
    for (size_t i = 0; i < (size_t)whole_digits; i++) {
        unsigned digit = decimal_digit(value, digits, i);
        if (magnitude > (limit - digit) / 10)
            return SQLI_OUT_OF_RANGE;
        magnitude = magnitude * 10 + digit;
    }
    if (!value->negative)
        *out = (int64_t)magnitude;
    else if (magnitude == (uint64_t)INT64_MAX + 1u)
        *out = INT64_MIN;
    else
        *out = -(int64_t)magnitude;
    return SQLI_OK;
}

sqli_status sqli_decimal_compare(const sqli_decimal_t *left,
                                 const sqli_decimal_t *right, int *out)
{
    if (left == NULL || right == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (left->is_null || right->is_null)
        return SQLI_NULL_VALUE;
    int ordering = 0;
    if (left->used == 0 || right->used == 0) {
        if (left->used != 0)
            ordering = left->negative ? -1 : 1;
        else if (right->used != 0)
            ordering = right->negative ? 1 : -1;
    } else if (left->negative != right->negative) {
        ordering = left->negative ? -1 : 1;
    } else {
        size_t left_digits = decimal_digits(left);
        size_t right_digits = decimal_digits(right);
        int64_t left_exponent = (int64_t)left_digits - left->scale;
        int64_t right_exponent = (int64_t)right_digits - right->scale;
        if (left_exponent != right_exponent) {
            ordering = left_exponent < right_exponent ? -1 : 1;
        } else {
            size_t count = left_digits > right_digits ? left_digits : right_digits;
            for (size_t i = 0; i < count; i++) {
                unsigned a = decimal_digit(left, left_digits, i);
                unsigned b = decimal_digit(right, right_digits, i);
                if (a != b) {
                    ordering = a < b ? -1 : 1;
                    break;
                }
            }
        }
        if (left->negative)
            ordering = -ordering;
    }
    *out = ordering;
    return SQLI_OK;
}

static sqli_status decimal_multiply_power(sqli_decimal_t *out,
                                         const sqli_decimal_t *value, int64_t power)
{
    size_t digits = decimal_digits(value);
    if (power > (int64_t)SQLI_DECIMAL_MAX_DIGITS - (int64_t)digits)
        return SQLI_LIMIT_EXCEEDED;
    size_t result_digits = digits + (size_t)power;
    size_t count = (result_digits + decimal_limb_digits - 1) / decimal_limb_digits;
    sqli_status status = decimal_reserve(out, count);
    if (status != SQLI_OK)
        return status;
    memset(out->limbs, 0, count * sizeof(*out->limbs));
    size_t shift = (size_t)power / decimal_limb_digits;
    uint32_t factor = powers_of_ten[(size_t)power % decimal_limb_digits];
    uint64_t carry = 0;
    for (size_t i = 0; i < value->used; i++) {
        uint64_t product = (uint64_t)value->limbs[i] * factor + carry;
        out->limbs[i + shift] = (uint32_t)(product % SQLI_DECIMAL_LIMB_BASE);
        carry = product / SQLI_DECIMAL_LIMB_BASE;
    }
    if (carry != 0)
        out->limbs[value->used + shift] = (uint32_t)carry;
    out->used = count;
    return SQLI_OK;
}

static sqli_status decimal_divide_power(sqli_decimal_t *out,
                                       const sqli_decimal_t *value, int64_t power)
{
    if (power >= (int64_t)decimal_digits(value))
        return SQLI_INEXACT;
    size_t shift = (size_t)power / decimal_limb_digits;
    uint32_t divisor = powers_of_ten[(size_t)power % decimal_limb_digits];
    for (size_t i = 0; i < shift; i++) {
        if (value->limbs[i] != 0)
            return SQLI_INEXACT;
    }
    if (value->limbs[shift] % divisor != 0)
        return SQLI_INEXACT;
    size_t count = value->used - shift;
    sqli_status status = decimal_reserve(out, count);
    if (status != SQLI_OK)
        return status;
    uint64_t remainder = 0;
    for (size_t i = value->used; i > shift; i--) {
        uint64_t dividend = remainder * SQLI_DECIMAL_LIMB_BASE + value->limbs[i - 1];
        out->limbs[i - shift - 1] = (uint32_t)(dividend / divisor);
        remainder = dividend % divisor;
    }
    out->used = count;
    return SQLI_OK;
}

sqli_status sqli_decimal_rescale_exact(sqli_decimal_t *value, int32_t scale)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (value->is_null)
        return SQLI_NULL_VALUE;
    if (value->scale == scale)
        return SQLI_OK;
    if (value->used == 0) {
        value->scale = scale;
        return SQLI_OK;
    }
    int64_t difference = (int64_t)scale - value->scale;
    sqli_decimal_t temporary;
    decimal_init(&temporary);
    sqli_status status = difference > 0 ? decimal_multiply_power(&temporary, value, difference) :
                                        decimal_divide_power(&temporary, value, -difference);
    if (status == SQLI_OK) {
        temporary.is_null = false;
        temporary.negative = value->negative;
        temporary.scale = scale;
        decimal_trim(&temporary);
        decimal_move(value, &temporary);
    }
    decimal_clear(&temporary);
    return status;
}

static bool ascii_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

static sqli_status parse_exponent(const char *text, size_t length, size_t position,
                                  int64_t *out)
{
    bool negative = false;
    if (position < length && (text[position] == '+' || text[position] == '-'))
        negative = text[position++] == '-';
    if (position == length)
        return SQLI_INVALID_ARGUMENT;
    int64_t exponent = 0;
    for (; position < length; position++) {
        if (!ascii_digit(text[position]))
            return SQLI_INVALID_ARGUMENT;
        int digit = text[position] - '0';
        if (exponent > (INT64_MAX - digit) / 10)
            return SQLI_OUT_OF_RANGE;
        exponent = exponent * 10 + digit;
    }
    *out = negative ? -exponent : exponent;
    return SQLI_OK;
}

sqli_status sqli_decimal_parse(sqli_decimal_t *value, const char *text,
                               size_t length, bool is_null)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (is_null)
        return sqli_decimal_set_null(value);
    if (text == NULL || length == 0)
        return SQLI_INVALID_ARGUMENT;
    if (length > SQLI_DECIMAL_MAX_TEXT)
        return SQLI_LIMIT_EXCEEDED;
    size_t position = 0;
    bool negative = false;
    if (text[position] == '+' || text[position] == '-')
        negative = text[position++] == '-';
    size_t start = position;
    size_t digits = 0;
    size_t fractional = 0;
    size_t significant_digits = 0;
    bool point = false;
    for (; position < length && text[position] != 'e' && text[position] != 'E'; position++) {
        char ch = text[position];
        if (ch == '.' && !point) {
            point = true;
        } else if (ascii_digit(ch)) {
            digits++;
            if (ch != '0' || significant_digits != 0)
                significant_digits++;
            if (point)
                fractional++;
        } else {
            return SQLI_INVALID_ARGUMENT;
        }
    }
    if (digits == 0)
        return SQLI_INVALID_ARGUMENT;
    if (significant_digits > SQLI_DECIMAL_MAX_DIGITS)
        return SQLI_LIMIT_EXCEEDED;
    size_t mantissa_end = position;
    int64_t exponent = 0;
    if (position < length) {
        sqli_status status = parse_exponent(text, length, position + 1, &exponent);
        if (status != SQLI_OK)
            return status;
    }
    if (exponent < (int64_t)fractional - INT32_MAX ||
        exponent > (int64_t)fractional - INT32_MIN)
        return SQLI_OUT_OF_RANGE;
    sqli_decimal_t temporary;
    decimal_init(&temporary);
    size_t count = (significant_digits + decimal_limb_digits - 1) / decimal_limb_digits;
    sqli_status status = decimal_reserve(&temporary, count);
    if (status != SQLI_OK)
        goto cleanup;
    memset(temporary.limbs, 0, count * sizeof(*temporary.limbs));
    size_t offset = 0;
    for (size_t i = mantissa_end; i > start && offset < significant_digits; i--) {
        char ch = text[i - 1];
        if (ch != '.') {
            temporary.limbs[offset / decimal_limb_digits] +=
                (uint32_t)(ch - '0') * powers_of_ten[offset % decimal_limb_digits];
            offset++;
        }
    }
    temporary.used = count;
    temporary.scale = (int32_t)((int64_t)fractional - exponent);
    temporary.negative = negative;
    temporary.is_null = false;
    decimal_trim(&temporary);
    decimal_move(value, &temporary);
cleanup:
    decimal_clear(&temporary);
    return status;
}

/* Build the unsigned exponent backwards; its bounds follow from int32 scale
 * and the coefficient limit. The buffer also has room for a full uint64. */
static size_t exponent_digits(int64_t exponent, char *buffer)
{
    uint64_t magnitude = exponent < 0 ? (uint64_t)(-exponent) : (uint64_t)exponent;
    size_t count = 0;
    do {
        buffer[count++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    return count;
}

sqli_status sqli_decimal_format(const sqli_decimal_t *value, char *buffer,
                                size_t capacity, size_t *required, bool *is_null)
{
    if (value == NULL || required == NULL || is_null == NULL ||
        (buffer == NULL && capacity != 0))
        return SQLI_INVALID_ARGUMENT;
    if (value->is_null) {
        *required = 0;
        *is_null = true;
        return SQLI_OK;
    }
    size_t digits = decimal_digits(value);
    int64_t exponent = (int64_t)digits - 1 - value->scale;
    bool scientific = value->scale < 0 || exponent < decimal_plain_min_exponent;
    char exponent_buffer[decimal_exponent_capacity];
    size_t exponent_length = scientific ? exponent_digits(exponent, exponent_buffer) : 0;
    /* All lengths are bounded by MAX_DIGITS plus a small notation overhead;
     * neither a large negative scale nor a tiny value expands into huge text. */
    size_t length = value->negative ? 1u : 0u;
    if (scientific)
        length += digits + (digits > 1 ? 1u : 0u) + 2 + exponent_length;
    else if (value->scale == 0)
        length += digits;
    else if ((size_t)value->scale < digits)
        length += digits + 1;
    else
        length += 2 + (size_t)value->scale;
    *required = length + 1;
    *is_null = false;
    if (buffer == NULL)
        return SQLI_OK;
    if (capacity < length + 1)
        return SQLI_BUFFER_TOO_SMALL;
    size_t position = 0;
    if (value->negative)
        buffer[position++] = '-';
    if (scientific) {
        for (size_t i = 0; i < digits; i++) {
            if (i == 1)
                buffer[position++] = '.';
            buffer[position++] = (char)('0' + decimal_digit(value, digits, i));
        }
        buffer[position++] = 'E';
        buffer[position++] = exponent < 0 ? '-' : '+';
        for (size_t i = exponent_length; i > 0; i--)
            buffer[position++] = exponent_buffer[i - 1];
    } else if (value->scale > 0 && (size_t)value->scale >= digits) {
        buffer[position++] = '0';
        buffer[position++] = '.';
        size_t zeros = (size_t)value->scale - digits;
        memset(buffer + position, '0', zeros);
        position += zeros;
        for (size_t i = 0; i < digits; i++)
            buffer[position++] = (char)('0' + decimal_digit(value, digits, i));
    } else {
        for (size_t i = 0; i < digits; i++) {
            if (value->scale != 0 && i == digits - (size_t)value->scale)
                buffer[position++] = '.';
            buffer[position++] = (char)('0' + decimal_digit(value, digits, i));
        }
    }
    buffer[position] = '\0';
    return SQLI_OK;
}
