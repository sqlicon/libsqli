#ifndef SQLI_TEMPORAL_INTERNAL_H
#define SQLI_TEMPORAL_INTERNAL_H

#include "libsqli/sqli_temporal.h"

/* Private storage also permits allocation-free convenience conversions. */
struct sqli_datetime { sqli_datetime_parts_t parts; };
struct sqli_interval { sqli_interval_parts_t parts; };

#endif
