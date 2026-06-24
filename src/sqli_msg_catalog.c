#include "sqli_msg_catalog.h"
#include "sqli_internal.h"

typedef struct {
    int code;
    const char *msg;
} code_msg_pair;

/* Extracted from com/informix/msg/sql_en_US.properties and
 * com/informix/msg/cals_en_US.properties. */
static const code_msg_pair k_sql_msgs[] = {
    {-201, "A syntax error has occurred near position: %s."},
    {-329, "Database (%s) not found or no system permission."},
    {-368, "Incompatible sqlexec module."},
    {-408, "Invalid message type received from the sqlexec process."},
    {-619, "A blob error has occurred in the front-end application."},
    {-908, "Socket connection to server (%s) failed. Check your server is reachable from this client on the host:port specified."},
    {-912, "Network error - Could not write to database server."},
    {-913, "Network error - Could not read from database server."},
    {-937, "User Defined Routine error."},
    {-23101, "Unable to load locale categories."},
    {-23197, "Database locale information mismatch."},
};

/* Extracted from com/informix/msg/isam_en_US.properties. */
static const code_msg_pair k_isam_msgs[] = {
    {100, "ISAM error:  duplicate value for a record with unique key."},
    {107, "ISAM error:  record is locked."},
    {111, "ISAM error:  no record found."},
    {126, "ISAM error: bad row id"},
    {143, "ISAM error: deadlock detected"},
    {149, "ISAM error: Informix Database Server daemon is no longer running"},
    {154, "ISAM error: Lock Timeout Expired"},
    {172, "ISAM error:  Unexpected internal error"},
    {190, "ISAM error: Transaction table overflow"},
    {7350, "Attempt to update a stale version of a row"},
    {7351, "Connection between secondary and primary has been lost"},
    {7352, "Operation (%s) can not be run on secondary node"},
    {7353, "The transaction cannot continue on the new primary server."},
};

static const char *lookup_msg(const code_msg_pair *arr, size_t n, int code)
{
    for (size_t i = 0; i < n; i++) {
        if (arr[i].code == code)
            return arr[i].msg;
    }
    return NULL;
}

const char *sqli_sql_message_lookup(int sqlcode)
{
    return lookup_msg(k_sql_msgs, sizeof(k_sql_msgs) / sizeof(k_sql_msgs[0]), sqlcode);
}

const char *sqli_isam_message_lookup(int isamcode)
{
    if (isamcode < 0)
        isamcode = -isamcode;
    return lookup_msg(k_isam_msgs, sizeof(k_isam_msgs) / sizeof(k_isam_msgs[0]), isamcode);
}

const char *sqli_opcode_name(uint16_t opcode)
{
    switch (opcode) {
    case SQLI_SQ_DBOPEN_FLAGS: return "SQ_DBOPEN_FLAGS";
    case SQLI_SQ_COMMAND: return "SQ_COMMAND";
    case SQLI_SQ_PREPARE: return "SQ_PREPARE";
    case SQLI_SQ_ID: return "SQ_ID";
    case SQLI_SQ_BIND: return "SQ_BIND";
    case SQLI_SQ_EXECUTE: return "SQ_EXECUTE";
    case SQLI_SQ_DESCRIBE: return "SQ_DESCRIBE";
    case SQLI_SQ_NFETCH: return "SQ_NFETCH";
    case SQLI_SQ_CLOSE: return "SQ_CLOSE";
    case SQLI_SQ_RELEASE: return "SQ_RELEASE";
    case SQLI_SQ_EOT: return "SQ_EOT";
    case SQLI_SQ_ERR: return "SQ_ERR";
    case SQLI_SQ_TUPLE: return "SQ_TUPLE";
    case SQLI_SQ_DONE: return "SQ_DONE";
    case SQLI_SQ_BEGIN: return "SQ_BEGIN";
    case SQLI_SQ_DBOPEN: return "SQ_DBOPEN";
    case SQLI_SQ_DBCLOSE: return "SQ_DBCLOSE";
    case SQLI_SQ_FETCHBLOB: return "SQ_FETCHBLOB";
    case SQLI_SQ_BLOB: return "SQ_BLOB";
    case SQLI_SQ_SVPOINT: return "SQ_SVPOINT";
    case SQLI_SQ_ISOLEVEL: return "SQ_ISOLEVEL";
    case SQLI_SQ_LOCKWAIT: return "SQ_LOCKWAIT";
    case SQLI_SQ_VERSION: return "SQ_VERSION";
    case SQLI_SQ_EXIT: return "SQ_EXIT";
    case SQLI_SQ_INFO: return "SQ_INFO";
    case SQLI_SQ_LODATA: return "SQ_LODATA";
    case SQLI_SQ_FILE: return "SQ_FILE";
    case SQLI_SQ_INSERTDONE: return "SQ_INSERTDONE";
    case SQLI_SQ_XACTSTAT: return "SQ_XACTSTAT";
    case SQLI_SQ_RET_TYPE: return "SQ_RET_TYPE";
    case SQLI_SQ_VPUT: return "SQ_VPUT";
    case SQLI_SQ_SQLISETSVPT: return "SQ_SQLISETSVPT";
    case SQLI_SQ_SQLIRELSVPT: return "SQ_SQLIRELSVPT";
    case SQLI_SQ_SQLIRBACKSVPT: return "SQ_SQLIRBACKSVPT";
    case SQLI_SQ_PROTOCOLS: return "SQ_PROTOCOLS";
    case SQLI_SQ_COST: return "SQ_COST";
    default: return "SQ_UNKNOWN";
    }
}
