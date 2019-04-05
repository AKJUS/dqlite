#ifndef DQLITE_H
#define DQLITE_H

#include <stdint.h>
#include <stdio.h>

#include <sqlite3.h>
#include <uv.h>

/* Error codes */
enum { DQLITE_BADSOCKET = 1,
       DQLITE_MISUSE,
       DQLITE_NOMEM,
       DQLITE_PROTO,
       DQLITE_PARSE,
       DQLITE_OVERFLOW,
       DQLITE_EOM,    /* End of message */
       DQLITE_ENGINE, /* A SQLite error occurred */
       DQLITE_NOTFOUND,
       DQLITE_STOPPED /* The server was stopped */
};

/* Current protocol version */
#define DQLITE_PROTOCOL_VERSION 0x86104dd760433fe5

/* Request types */
#define DQLITE_REQUEST_LEADER 0
#define DQLITE_REQUEST_CLIENT 1
#define DQLITE_REQUEST_HEARTBEAT 2
#define DQLITE_REQUEST_OPEN 3
#define DQLITE_REQUEST_PREPARE 4
#define DQLITE_REQUEST_EXEC 5
#define DQLITE_REQUEST_QUERY 6
#define DQLITE_REQUEST_FINALIZE 7
#define DQLITE_REQUEST_EXEC_SQL 8
#define DQLITE_REQUEST_QUERY_SQL 9
#define DQLITE_REQUEST_INTERRUPT 10

/* Response types */
#define DQLITE_RESPONSE_FAILURE 0
#define DQLITE_RESPONSE_SERVER 1
#define DQLITE_RESPONSE_WELCOME 2
#define DQLITE_RESPONSE_SERVERS 3
#define DQLITE_RESPONSE_DB 4
#define DQLITE_RESPONSE_STMT 5
#define DQLITE_RESPONSE_RESULT 6
#define DQLITE_RESPONSE_ROWS 7
#define DQLITE_RESPONSE_EMPTY 8

/* Special datatypes */
#define DQLITE_UNIXTIME 9
#define DQLITE_ISO8601 10
#define DQLITE_BOOLEAN 11

/* Log levels */
enum { DQLITE_DEBUG = 0, DQLITE_INFO, DQLITE_WARN, DQLITE_ERROR };

/* Config opcodes */
#define DQLITE_CONFIG_LOGGER 0
#define DQLITE_CONFIG_VFS 1
#define DQLITE_CONFIG_WAL_REPLICATION 2
#define DQLITE_CONFIG_HEARTBEAT_TIMEOUT 3
#define DQLITE_CONFIG_PAGE_SIZE 4
#define DQLITE_CONFIG_CHECKPOINT_THRESHOLD 5
#define DQLITE_CONFIG_METRICS 6

/* Special value indicating that a batch of rows is over, but there are more. */
#define DQLITE_RESPONSE_ROWS_PART 0xeeeeeeeeeeeeeeee

/* Special value indicating that the result set is complete. */
#define DQLITE_RESPONSE_ROWS_DONE 0xffffffffffffffff

/**
 * Initialize SQLite global state with values specific to dqlite
 *
 * This API must be called exactly once before any other SQLite or dqlite API
 * call is invoked in a process.
 */
int dqlite_initialize();

/* Interface implementing cluster-related functionality */
typedef struct dqlite_info
{
	uint64_t id;
	const char *address;
} dqlite_info;

/* Handle connections from dqlite clients */
typedef struct dqlite dqlite;

/* Allocate and initialize a dqlite server instance. */
int dqlite_create(const char *dir,
		  unsigned id,
		  const char *address,
		  dqlite **out);

/* Destroy and deallocate a dqlite server instance. */
void dqlite_destroy(dqlite *s);

/* Set a config option on a dqlite server
 *
 * This API must be called after dqlite_init and before
 * dqlite_run.
 */
int dqlite_config(dqlite *s, int op, void *arg);

int dqlite_bootstrap(dqlite *s);

/* Start a dqlite server.
 *
 * In case of error, a human-readable message describing the failure can be
 * obtained using dqlite_errmsg.
 */
int dqlite_run(dqlite *s);

/* Wait until a dqlite server is ready and can handle connections.
**
** Returns 1 if the server has been successfully started, 0 otherwise.
**
** This is a thread-safe API, but must be invoked before any call to
** dqlite_stop or dqlite_handle.
*/
int dqlite_ready(dqlite *s);

/* Stop a dqlite server and wait for it to shutdown.
**
** This is a thread-safe API.
**
** In case of error, the caller must invoke sqlite3_free
** against the returned errmsg.
*/
int dqlite_stop(dqlite *s, char **errmsg);

/* Start handling a new connection.
**
** This is a thread-safe API.
**
** In case of error, the caller must invoke sqlite3_free
** against the returned errmsg.
*/
int dqlite_handle(dqlite *s, int socket, char **errrmsg);

/* Return a message describing the most recent error occurred.
 *
 * This is API not thread-safe.
 *
 * The memory holding returned string is managed by the dqlite object
 * internally, and will be valid until dqlite_close is invoked. However,
 * the message contained in the string itself might change if another error
 * occurs in the meantime.
 */
const char *dqlite_errmsg(dqlite *s);

#endif /* DQLITE_H */
