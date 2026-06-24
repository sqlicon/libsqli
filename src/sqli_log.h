#ifndef SQLI_LOG_H
#define SQLI_LOG_H

/*
 * SQLI logging abstraction — internal, not exposed in public header.
 *
 * All internal code must use this interface, never printf/fprintf directly.
 * The concrete backend can be swapped without touching business logic.
 */

#include "sqli_internal.h"

/* Log a message. Implementations must be thread-safe. */
void sqli_log(sqli_log_level level, const char *fmt, ...);

/* Set the minimum log level. Default: SQLI_LOG_ERROR. */
void sqli_log_set_level(sqli_log_level level);

#endif /* SQLI_LOG_H */
