#include "sqli_internal.h"

#include "sqli_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/*
 * This file was refactored in Phase 2.
 * All logic moved to sqli_conn.c and sqli_handshake.c.
 * This file is kept as an empty compilation unit to avoid
 * CMake configuration changes.
 */

/* Suppress unused-include warning */
typedef int _keep_compilation_unit;
