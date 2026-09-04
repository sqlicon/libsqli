#ifndef SQLI_MSG_CATALOG_H
#define SQLI_MSG_CATALOG_H

#include <stdint.h>
#include <stddef.h>

const char *sqli_sql_message_lookup(int sqlcode);
const char *sqli_isam_message_lookup(int isamcode);
int sqli_catalog_message_get(int32_t code, char *out, size_t outsz);
const char *sqli_opcode_name(uint16_t opcode);

#endif /* SQLI_MSG_CATALOG_H */
