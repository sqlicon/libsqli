#ifndef SQLI_PLATFORM_H
#define SQLI_PLATFORM_H

#ifdef _WIN32
#include "sqli_platform_win.h"
#else
#include "sqli_platform_posix.h"
#endif

#endif /* SQLI_PLATFORM_H */
