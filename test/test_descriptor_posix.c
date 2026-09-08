#include "libsqli/sqli.h"
#include "unity.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sqli_descriptor_internal.h"
#include "sqli_internal.h"

enum { frame_capacity = 4096, long_name_length = 300, type_string_length = 257 };
static sqli_result_t result;
static sqli_descriptor_t *held;
static sqli_descriptor_t *second;
static int sockets[2];
static sqli_conn_t *connection;

void setUp(void)
{
    memset(&result, 0, sizeof(result));
    sockets[0] = sockets[1] = -1;
}
void tearDown(void)
{
    sqli_result_cleanup(&result);
    sqli_descriptor_release(held);
    sqli_descriptor_release(second);
    held = second = NULL;
    sqli_destroy(connection);
    connection = NULL;
    for (size_t i = 0; i < 2; i++) {
        if (sockets[i] >= 0)
            close(sockets[i]);
        sockets[i] = -1;
    }
}

static void word(uint8_t *frame, size_t *position, uint16_t value)
{
    TEST_ASSERT_TRUE(*position <= frame_capacity - 2);
    frame[(*position)++] = (uint8_t)(value >> 8);
    frame[(*position)++] = (uint8_t)value;
}
static void dword(uint8_t *frame, size_t *position, uint32_t value)
{
    word(frame, position, (uint16_t)(value >> 16));
    word(frame, position, (uint16_t)value);
}
static void header(uint8_t *frame, size_t *position, bool extended, uint16_t fields, uint32_t names)
{
    word(frame, position, SQLI_SQ_DESCRIBE);
    word(frame, position, 2);
    word(frame, position, 42);
    dword(frame, position, UINT32_C(0xdeadbeef));
    word(frame, position, 8);
    word(frame, position, fields);
    if (extended)
        dword(frame, position, names);
    else {
        TEST_ASSERT_TRUE(names <= UINT16_MAX);
        word(frame, position, (uint16_t)names);
    }
}
static void tail(uint8_t *frame, size_t *position)
{
    word(frame, position, SQLI_SQ_DONE);
    word(frame, position, 0);
    dword(frame, position, 0);
    dword(frame, position, 0);
    dword(frame, position, 0);
    word(frame, position, SQLI_SQ_EOT);
}
static sqli_status receive(const uint8_t *frame, size_t length, bool extended)
{
    sqli_destroy(connection);
    connection = NULL;
    for (size_t i = 0; i < 2; i++) {
        if (sockets[i] >= 0)
            close(sockets[i]);
        sockets[i] = -1;
    }
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&connection));
    connection->caps.extended_describe = extended;
    result.saw_done = false;
    result.saw_error = false;
    result.eof = 0;
    TEST_ASSERT_TRUE(length <= frame_capacity);
    ssize_t written = write(sockets[1], frame, length);
    TEST_ASSERT_TRUE(written >= 0);
    TEST_ASSERT_EQUAL_UINT(length, (size_t)written);
    TEST_ASSERT_EQUAL_INT(0, shutdown(sockets[1], SHUT_WR));
    return sqli_receive_dispatch(sockets[0], &result, connection);
}
static size_t legacy_frame(uint8_t *frame, bool malformed)
{
    size_t position = 0;
    const uint8_t names[] = {0, 'b', 0};
    header(frame, &position, false, 2, sizeof(names));
    for (unsigned i = 0; i < 2; i++) {
        dword(frame, &position, UINT32_MAX - i);
        dword(frame, &position, i * 4);
        word(frame, &position, 0x89fe);
        word(frame, &position, 0x1234);
    }
    memcpy(frame + position, names, sizeof(names));
    position += sizeof(names);
    if (malformed)
        frame[position - 1] = 'x';
    frame[position++] = 0;
    tail(frame, &position);
    return position;
}
static size_t extended_frame(uint8_t *frame)
{
    size_t position = 0;
    header(frame, &position, true, 1, long_name_length + 3);
    dword(frame, &position, UINT32_MAX);
    dword(frame, &position, UINT32_C(0xfedcba98));
    word(frame, &position, 0x89fe);
    dword(frame, &position, UINT32_C(0x11223344));
    for (unsigned i = 0; i < 2; i++) {
        word(frame, &position, type_string_length);
        memset(frame + position, i == 0 ? 'o' : 't', type_string_length);
        frame[position + 1] = 0; /* Embedded NUL stays in the raw byte view. */
        position += type_string_length;
        frame[position++] = 0;
    }
    word(frame, &position, 0xabcd);
    word(frame, &position, 0x1234);
    dword(frame, &position, UINT32_C(0xf1234567));
    dword(frame, &position, UINT32_C(0x89abcdef));
    memset(frame + position, 'n', long_name_length);
    position += long_name_length;
    frame[position++] = 0;
    frame[position++] = 0x81; /* Uninterpreted trailing table bytes are retained. */
    frame[position++] = 0x82;
    frame[position++] = 0;
    tail(frame, &position);
    return position;
}

static void test_extended_fields_and_lifetime(void)
{
    uint8_t frame[frame_capacity];
    size_t length = extended_frame(frame);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, receive(frame, length, true));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_descriptor(&result, &held));
    sqli_descriptor_info_t info;
    const sqli_descriptor_field_t *field;
    info = held->info;
    TEST_ASSERT_TRUE(info.extended);
    TEST_ASSERT_EQUAL_UINT32(0xdeadbeef, info.cost_raw);
    TEST_ASSERT_EQUAL_UINT(42, info.statement_id);
    TEST_ASSERT_EQUAL_UINT(8, info.tuple_size);
    TEST_ASSERT_EQUAL_UINT(1, info.field_count);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field(held, 0, &field));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, field->field_index);
    TEST_ASSERT_EQUAL_UINT32(0xfedcba98, field->tuple_offset);
    TEST_ASSERT_EQUAL_UINT(0x89fe, field->type_raw);
    TEST_ASSERT_EQUAL_UINT32(0x11223344, field->extended_info);
    TEST_ASSERT_EQUAL_UINT(0xabcd, field->reference);
    TEST_ASSERT_EQUAL_UINT(0x1234, field->alignment);
    TEST_ASSERT_EQUAL_UINT32(0xf1234567, field->source_type);
    TEST_ASSERT_EQUAL_UINT32(0x89abcdef, field->encoded_length);
    TEST_ASSERT_TRUE(field->name.available);
    TEST_ASSERT_EQUAL_UINT(long_name_length, field->name.length);
    TEST_ASSERT_EACH_EQUAL_UINT8('n', field->name.data, field->name.length);
    TEST_ASSERT_EQUAL_UINT(type_string_length, field->type_owner.length);
    TEST_ASSERT_EQUAL_UINT(type_string_length, field->type_name.length);
    TEST_ASSERT_EQUAL_UINT8(0, field->type_owner.data[1]);
    TEST_ASSERT_EQUAL_UINT8('t', field->type_name.data[type_string_length - 1]);
    TEST_ASSERT_EQUAL_UINT(127, strlen(result.columns[0].name));
    sqli_descriptor_bytes_t names;
    names = (sqli_descriptor_bytes_t){held->names, held->names_length, true};
    TEST_ASSERT_EQUAL_UINT(long_name_length + 3, names.length);
    TEST_ASSERT_EQUAL_UINT8(0x82, names.data[names.length - 1]);
    memset(frame, 0, sizeof(frame));
    result.columns[0].col_start_pos = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field(held, 0, &field));
    TEST_ASSERT_EQUAL_UINT32(0xfedcba98, field->tuple_offset);
    TEST_ASSERT_EQUAL_UINT8('o', field->type_owner.data[0]);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_retain(held));
    second = held;
    sqli_result_cleanup(&result);
    sqli_destroy(connection);
    connection = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field(second, 0, &field));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_name(field, &names));
    TEST_ASSERT_EACH_EQUAL_UINT8('n', names.data, names.length);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_type_owner(field, &names));
    TEST_ASSERT_EQUAL_UINT(type_string_length, names.length);
    TEST_ASSERT_EQUAL_UINT8(0, names.data[1]);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_type_name(field, &names));
    TEST_ASSERT_EQUAL_UINT8('t', names.data[type_string_length - 1]);
}

static void test_empty_missing_names_and_availability(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_result_get_descriptor(&result, &held));
    uint8_t frame[frame_capacity];
    size_t length = legacy_frame(frame, false);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, receive(frame, length, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_descriptor(&result, &held));
    const sqli_descriptor_field_t *field;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field(held, 0, &field));
    TEST_ASSERT_TRUE(field->name.available);
    TEST_ASSERT_EQUAL_UINT(0, field->name.length);
    TEST_ASSERT_FALSE(field->type_name.available);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field(held, 1, &field));
    TEST_ASSERT_EQUAL_UINT(1, field->name.length);
    TEST_ASSERT_EQUAL_UINT8('b', field->name.data[0]);
    length = 0;
    header(frame, &length, false, 1, 0);
    dword(frame, &length, 0);
    dword(frame, &length, 0);
    word(frame, &length, SQLI_TYPE_INT);
    word(frame, &length, 4);
    tail(frame, &length);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, receive(frame, length, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_descriptor(&result, &second));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field(second, 0, &field));
    TEST_ASSERT_FALSE(field->name.available);
    sqli_descriptor_release(second);
    second = NULL;
    length = 0;
    header(frame, &length, false, 0, 0);
    tail(frame, &length);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, receive(frame, length, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_descriptor(&result, &second));
    sqli_descriptor_info_t info;
    info = second->info;
    TEST_ASSERT_EQUAL_UINT(0, info.field_count);
    info = held->info;
    TEST_ASSERT_EQUAL_UINT(2, info.field_count);
    sqli_result_cleanup(&result);
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_result_get_descriptor(&result, &second));
    TEST_ASSERT_NOT_NULL(second); /* Failed acquisition did not overwrite ownership. */
}

static void test_malformed_replacement_preserves_snapshot(void)
{
    uint8_t frame[frame_capacity];
    size_t length = legacy_frame(frame, false);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, receive(frame, length, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_descriptor(&result, &held));
    sqli_column_info *columns = result.columns;
    length = legacy_frame(frame, true);
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, receive(frame, length, false));
    TEST_ASSERT_EQUAL_PTR(held, result.descriptor);
    TEST_ASSERT_EQUAL_PTR(columns, result.columns);
    TEST_ASSERT_EQUAL_INT(2, result.column_count);
    length = extended_frame(frame);
    /* Every prefix inside the descriptor must fail without replacing it.
     * Stop before DONE/EOT, whose absence is a separate dispatch concern. */
    enum { completion_length = 18 };
    for (size_t cut = 2; cut < length - completion_length; cut++) {
        TEST_ASSERT_NOT_EQUAL(SQLI_OK, receive(frame, cut, true));
        TEST_ASSERT_EQUAL_PTR(held, result.descriptor);
        TEST_ASSERT_EQUAL_PTR(columns, result.columns);
    }
    length = 0;
    header(frame, &length, true, 1, UINT32_MAX);
    TEST_ASSERT_EQUAL_INT(SQLI_LIMIT_EXCEEDED, receive(frame, length, true));
    TEST_ASSERT_EQUAL_PTR(held, result.descriptor);
}

static void test_argument_and_reference_limits(void)
{
    sqli_descriptor_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_create(&info, &held));
    sqli_descriptor_field_t sentinel = {.field_index = 7};
    const sqli_descriptor_field_t *field = &sentinel;
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_descriptor_get_field(held, 0, &field));
    TEST_ASSERT_EQUAL_UINT(7, field->field_index);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_get_field_count(held, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_field_get_name(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_retain(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_stmt_get_descriptor(NULL, &second));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_result_get_descriptor(NULL, &second));
    atomic_store(&held->references, SIZE_MAX);
    TEST_ASSERT_EQUAL_INT(SQLI_LIMIT_EXCEEDED, sqli_descriptor_retain(held));
    atomic_store(&held->references, 1);
    info.field_count = SIZE_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_LIMIT_EXCEEDED, sqli_descriptor_create(&info, &second));
    TEST_ASSERT_NULL(second);
    sqli_descriptor_release(NULL);
}

struct reference_worker { sqli_descriptor_t *descriptor; bool ok; };
static void *exercise_reference(void *argument)
{
    struct reference_worker *worker = argument;
    enum { repetitions = 10000 };
    worker->ok = true;
    for (unsigned i = 0; i < repetitions; i++) {
        if (sqli_descriptor_retain(worker->descriptor) != SQLI_OK) {
            worker->ok = false;
            break;
        }
        size_t count;
        if (sqli_descriptor_get_field_count(worker->descriptor, &count) != SQLI_OK || count != 0)
            worker->ok = false;
        sqli_descriptor_release(worker->descriptor);
    }
    return NULL;
}
static void test_concurrent_reference_ownership(void)
{
    const sqli_descriptor_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_create(&info, &held));
    enum { workers = 4 };
    pthread_t threads[workers];
    struct reference_worker state[workers] = {0};
    size_t started = 0;
    for (; started < workers; started++) {
        state[started].descriptor = held;
        if (pthread_create(&threads[started], NULL, exercise_reference, &state[started]) != 0)
            break;
    }
    bool ok = started == workers;
    for (size_t i = 0; i < started; i++) {
        int status = pthread_join(threads[i], NULL);
        ok = ok && status == 0 && state[i].ok;
    }
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT(1, atomic_load(&held->references));
}

static void test_semantic_properties_and_failure_atomicity(void)
{
    const sqli_descriptor_info_t info = {.field_count = 1};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_create(&info, &held));
    /* Private builder inputs stand in for received qualifiers. Public callers
     * only see the const field view acquired below. */
    sqli_descriptor_field_t *raw = &held->fields[0];
    const sqli_descriptor_field_t *field = NULL;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field_count(held, &count));
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field(held, 0, &field));
    TEST_ASSERT_EQUAL_PTR(raw, field);
    raw->type_raw = SQLI_TYPE_DECIMAL | SQLI_BIT_NOTNULLABLE;
    raw->encoded_length = 0x0802;
    sqli_column_type type = SQLI_TYPE_INT;
    uint8_t precision = 99, scale = 99;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_type(field, &type));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_DECIMAL, type);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_precision(field, &precision));
    TEST_ASSERT_EQUAL_UINT8(8, precision);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_scale(field, &scale));
    TEST_ASSERT_EQUAL_UINT8(2, scale);
    raw->encoded_length = 0x08ff;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_precision(field, &precision));
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_descriptor_field_get_scale(field, &scale));
    TEST_ASSERT_EQUAL_UINT8(2, scale);
    raw->encoded_length = 0x0809;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_descriptor_field_get_precision(field, &precision));
    TEST_ASSERT_EQUAL_UINT8(8, precision);
    raw->encoded_length = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_descriptor_field_get_scale(field, &scale));
    TEST_ASSERT_EQUAL_UINT8(2, scale);
    raw->type_raw |= SQLI_BIT_DISTINCT;
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_descriptor_field_get_type(field, &type));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_DECIMAL, type);
    raw->type_raw = 0xfe;
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_descriptor_field_get_type(field, &type));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_DECIMAL, type);
    raw->type_raw = SQLI_TYPE_INT;
    raw->extended = true;
    raw->extended_info = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_descriptor_field_get_type(field, &type));
    raw->extended_info = 10; /* Server extended BLOB identifier. */
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_type(field, &type));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_BLOB, type);
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_descriptor_field_get_scale(field, &scale));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_field_get_type(NULL, &type));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_field_get_precision(field, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_field_get_scale(NULL, &scale));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_get_field(held, 0, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_descriptor_get_field(held, 1, &field));
    TEST_ASSERT_EQUAL_PTR(raw, field);
}

static void test_temporal_properties_and_name_availability(void)
{
    const sqli_descriptor_info_t info = {.field_count = 1};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_create(&info, &held));
    sqli_descriptor_field_t *raw = &held->fields[0];
    const sqli_descriptor_field_t *field;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field(held, 0, &field));
    raw->type_raw = SQLI_TYPE_DATETIME;
    raw->encoded_length = 0x130f; /* YEAR TO FRACTION(5). */
    sqli_temporal_range_t range = {0};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_temporal_range(field, &range));
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_YEAR, range.first);
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_FRACTION, range.last);
    TEST_ASSERT_EQUAL_UINT8(5, range.fractional_digits);
    raw->type_raw = SQLI_TYPE_INTERVAL;
    raw->encoded_length = 0x094a; /* DAY(3) TO SECOND. */
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_temporal_range(field, &range));
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_DAY, range.first);
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_SECOND, range.last);
    TEST_ASSERT_EQUAL_UINT8(0, range.fractional_digits);
    raw->encoded_length = 0x130f; /* INTERVAL cannot mix YEAR and SECOND. */
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_descriptor_field_get_temporal_range(field, &range));
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_DAY, range.first);
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_SECOND, range.last);
    raw->encoded_length = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_descriptor_field_get_temporal_range(field, &range));
    raw->type_raw = SQLI_TYPE_INT;
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_descriptor_field_get_temporal_range(field, &range));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_field_get_temporal_range(NULL, &range));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_field_get_temporal_range(field, NULL));
    sqli_descriptor_bytes_t bytes = {NULL, 99, false};
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_descriptor_field_get_name(field, &bytes));
    TEST_ASSERT_EQUAL_UINT(99, bytes.length);
    raw->name.available = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_name(field, &bytes));
    TEST_ASSERT_TRUE(bytes.available);
    TEST_ASSERT_EQUAL_UINT(0, bytes.length);
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_descriptor_field_get_type_owner(field, &bytes));
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_descriptor_field_get_type_name(field, &bytes));
    TEST_ASSERT_TRUE(bytes.available);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_field_get_type_owner(NULL, &bytes));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_descriptor_field_get_type_name(field, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_semantic_properties_and_failure_atomicity);
    RUN_TEST(test_temporal_properties_and_name_availability);
    RUN_TEST(test_extended_fields_and_lifetime);
    RUN_TEST(test_empty_missing_names_and_availability);
    RUN_TEST(test_malformed_replacement_preserves_snapshot);
    RUN_TEST(test_argument_and_reference_limits);
    RUN_TEST(test_concurrent_reference_ownership);
    return UNITY_END();
}
