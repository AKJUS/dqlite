#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include "../lib/fs.h"
#include "../lib/heap.h"
#include "../lib/runner.h"
#include "../lib/server.h"
#include "../lib/sqlite.h"

#include "../../include/dqlite.h"
#include "../../src/protocol.h"
#include "../../src/server.h"
#include "../../src/utils.h"

#define CLIENT_N 20

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

static char *bools[] = {"0", "1", NULL};

static MunitParameterEnum node_params[] = {
    {"disk_mode", bools},
    {SNAPSHOT_COMPRESSION_PARAM, bools},
    {NULL, NULL},
};

struct fixture
{
	char *dir;         /* Data directory. */
	dqlite_node *node; /* Node instance. */
};

static void *setUp(const MunitParameter params[], void *user_data)
{
	struct fixture *f = munit_malloc(sizeof *f);
	int rv;
	test_heap_setup(params, user_data);
	test_sqlite_setup(params);

	f->dir = test_dir_setup();

	rv = dqlite_node_create(1, "1", f->dir, &f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_bind_address(f->node, "@123");
	munit_assert_int(rv, ==, 0);

	const char *disk_mode_param = munit_parameters_get(params, "disk_mode");
	if (disk_mode_param != NULL) {
		bool disk_mode = (bool)atoi(disk_mode_param);
		if (disk_mode) {
			rv = dqlite_node_enable_disk_mode(f->node);
			munit_assert_int(rv, ==, 0);
		}
	}

	return f;
}

static void *setUpInet(const MunitParameter params[], void *user_data)
{
	struct fixture *f = munit_malloc(sizeof *f);
	int rv;
	test_heap_setup(params, user_data);
	test_sqlite_setup(params);

	f->dir = test_dir_setup();

	rv = dqlite_node_create(1, "1", f->dir, &f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_bind_address(f->node, "127.0.0.1:9001");
	munit_assert_int(rv, ==, 0);

	const char *disk_mode_param = munit_parameters_get(params, "disk_mode");
	if (disk_mode_param != NULL) {
		bool disk_mode = (bool)atoi(disk_mode_param);
		if (disk_mode) {
			rv = dqlite_node_enable_disk_mode(f->node);
			munit_assert_int(rv, ==, 0);
		}
	}

	return f;
}

/* Tests if node starts/stops successfully and also performs some memory cleanup
 */
static void startStopNode(struct fixture *f)
{
	munit_assert_int(dqlite_node_start(f->node), ==, 0);
	munit_assert_int(dqlite_node_stop(f->node), ==, 0);
}

/* Recovery only works if a node has been started regularly for a first time. */
static void *setUpForRecovery(const MunitParameter params[], void *user_data)
{
	int rv;
	struct fixture *f = setUp(params, user_data);
	startStopNode(f);
	dqlite_node_destroy(f->node);

	rv = dqlite_node_create(1, "1", f->dir, &f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_bind_address(f->node, "@123");
	munit_assert_int(rv, ==, 0);

	const char *disk_mode_param = munit_parameters_get(params, "disk_mode");
	if (disk_mode_param != NULL) {
		bool disk_mode = (bool)atoi(disk_mode_param);
		if (disk_mode) {
			rv = dqlite_node_enable_disk_mode(f->node);
			munit_assert_int(rv, ==, 0);
		}
	}

	return f;
}

static void tearDown(void *data)
{
	struct fixture *f = data;

	dqlite_node_destroy(f->node);

	test_dir_tear_down(f->dir);
	test_sqlite_tear_down();
	test_heap_tear_down(data);
	free(f);
}

SUITE(node);

/******************************************************************************
 *
 * dqlite_node_start
 *
 ******************************************************************************/

TEST(node, start, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	return MUNIT_OK;
}

TEST(node, startInet, setUpInet, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	return MUNIT_OK;
}

TEST(node, startInetStopTwice, setUpInet, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, EBADF);

	return MUNIT_OK;
}

static int openDb(struct client_proto *c, struct dqlite_node *n, const char *db)
{
	int rv;
	*c = (struct client_proto) {
		.connect = n->connect_func,
		.connect_arg = n->connect_func_arg,
	};
	rv = clientOpen(c, n->bind_address, n->config.id);
	if (rv != RAFT_OK) {
		return rv;
	}
	rv = clientSendHandshake(c, NULL);
	if (rv != RAFT_OK) {
		clientClose(c);
		return rv;
	}
	rv = clientSendOpen(c, db, NULL);
	if (rv != RAFT_OK) {
		clientClose(c);
		return rv;
	}
	rv = clientRecvDb(c, NULL);
	if (rv != RAFT_OK) {
		clientClose(c);
		return rv;
	}
	return RAFT_OK;
}


TEST(node, stopInflightReads, setUpInet, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;
	rv = dqlite_node_set_busy_timeout(f->node, 10*1000);
	munit_assert_int(rv, ==, 0);
	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	/* Open a bunch of clients and do a query on them */
	struct client_proto clients[CLIENT_N];
	for (int i = 0; i < CLIENT_N; i++) {
		struct rows rows;
		bool done;

		rv = openDb(&clients[i], f->node, "test");
		munit_assert_int(rv, ==, RAFT_OK);
		rv = clientSendQuerySQL(&clients[i], 
			"WITH RECURSIVE inf(i) AS( "
			"    SELECT 1              "
			"	UNION ALL              "
			"	SELECT i+1 FROM inf    "
			")                         "
			"SELECT * FROM inf         ",
			NULL, 0, NULL
		);
		munit_assert_int(rv, ==, RAFT_OK);
		
		rv = clientRecvRows(&clients[i], &rows, &done, NULL);
		munit_assert_int(rv, ==, RAFT_OK);
		munit_assert_false(done);
		munit_assert_int(rows.column_count, ==, 1);
		munit_assert_string_equal(rows.column_names[0], "i");
		clientCloseRows(&rows);
	}

	/* Make sure the node can be closed */
	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	for (int i = 0; i < CLIENT_N; i++) {
		clientClose(&clients[i]);
	}
	return MUNIT_OK;
}

static void notify_transaction(sqlite3_context *context, int argc, sqlite3_value **argv);
static int register_notify(sqlite3 *connection, char **pzErrMsg, struct sqlite3_api_routines *pThunk)
{
	(void)pzErrMsg;
	(void)pThunk;
	return sqlite3_create_function_v2(connection, "notify_transaction", 1,
					  SQLITE_UTF8, NULL, notify_transaction,
					  NULL, NULL, NULL);
}

static sem_t write_sem;
static void notify_transaction(sqlite3_context *context, int argc, sqlite3_value **argv)
{
	(void)argc;
	/* Just return the same value */
	sqlite3_result_value(context, argv[0]);
	/* Signal the application that we were able to get to the write part */
	sem_post(&write_sem);
}

static void *setUpInflight(const MunitParameter params[], void *user_data)
{
	void *fixture = setUpInet(params, user_data);;

	int rv = sem_init(&write_sem, 0, 0);
	munit_assert_int(rv, ==, 0);

	rv = sqlite3_auto_extension((void (*)(void))register_notify);
	munit_assert_int(rv, ==, SQLITE_OK);

	return fixture;
}

static void tearDownInflight(void *data)
{
	int rv = sqlite3_cancel_auto_extension((void (*)(void))notify_transaction);
	munit_assert_int(rv, ==, SQLITE_OK);

	rv = sem_destroy(&write_sem);
	munit_assert_int(rv, ==, 0);

	tearDown(data);
}

TEST(node, stopInflightWrites, setUpInflight, tearDownInflight, 0, node_params)
{
	struct fixture *f = data;
	int started;
	int rv;
	rv = dqlite_node_set_busy_timeout(f->node, 10*1000);
	munit_assert_int(rv, ==, 0);
	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	/* Open a bunch of clients and do a query on them */
	struct client_proto clients[CLIENT_N];
	for (int i = 0; i < CLIENT_N; i++) {
		rv = openDb(&clients[i], f->node, "test");
		munit_assert_int(rv, ==, RAFT_OK);
	}

	rv = clientSendExecSQL(&clients[0], "CREATE TABLE test_table(i)", NULL, 0, NULL);
	munit_assert_int(rv, ==, RAFT_OK);
	rv = clientRecvResult(&clients[0], NULL, NULL, NULL);
	munit_assert_int(rv, ==, RAFT_OK);

	rv = sem_getvalue(&write_sem, &started);
	munit_assert_int(rv, ==, 0);
	munit_assert_int(started, ==, 0);

	for (int i = 0; i < CLIENT_N; i++) {
		rv = clientSendExecSQL(&clients[i], 
			"INSERT INTO test_table VALUES (notify_transaction(1))", NULL, 0, NULL);
		munit_assert_int(rv, ==, RAFT_OK);
	}

	sem_wait(&write_sem);

	/* Make sure the node can be closed */
	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	rv = sem_getvalue(&write_sem, &started);
	munit_assert_int(rv, ==, 0);
	/* Check that some of the queries were still in flight */
	munit_assert_int(started, <, CLIENT_N-1);

	for (int i = 0; i < CLIENT_N; i++) {
		clientClose(&clients[i]);
	}
	return MUNIT_OK;
}


TEST(node, snapshotParams, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_set_snapshot_params(f->node, 2048, 2048);
	munit_assert_int(rv, ==, 0);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, snapshotParamsRunning, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_snapshot_params(f->node, 2048, 2048);
	munit_assert_int(rv, !=, 0);

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	return MUNIT_OK;
}

TEST(node, snapshotParamsTrailingTooSmall, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_set_snapshot_params(f->node, 2, 2);
	munit_assert_int(rv, !=, 0);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node,
     snapshotParamsThresholdLargerThanTrailing,
     setUp,
     tearDown,
     0,
     node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_set_snapshot_params(f->node, 2049, 2048);
	munit_assert_int(rv, !=, 0);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, networkLatency, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_set_network_latency(f->node, 3600000000000ULL);
	munit_assert_int(rv, ==, 0);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, networkLatencyRunning, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_network_latency(f->node, 3600000000000ULL);
	munit_assert_int(rv, ==, DQLITE_MISUSE);

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	return MUNIT_OK;
}

TEST(node, networkLatencyTooLarge, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_set_network_latency(f->node, 3600000000000ULL + 1ULL);
	munit_assert_int(rv, ==, DQLITE_MISUSE);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, networkLatencyMs, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_set_network_latency_ms(f->node, 5);
	munit_assert_int(rv, ==, 0);
	rv = dqlite_node_set_network_latency_ms(f->node, (3600U * 1000U));
	munit_assert_int(rv, ==, 0);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, networkLatencyMsRunning, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_network_latency_ms(f->node, 2);
	munit_assert_int(rv, ==, DQLITE_MISUSE);

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	return MUNIT_OK;
}

TEST(node, networkLatencyMsTooSmall, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_set_network_latency_ms(f->node, 0);
	munit_assert_int(rv, ==, DQLITE_MISUSE);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, networkLatencyMsTooLarge, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_set_network_latency_ms(f->node, (3600U * 1000U) + 1);
	munit_assert_int(rv, ==, DQLITE_MISUSE);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, blockSize, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_set_block_size(f->node, 0);
	munit_assert_int(rv, ==, DQLITE_ERROR);
	rv = dqlite_node_set_block_size(f->node, 1);
	munit_assert_int(rv, ==, DQLITE_ERROR);
	rv = dqlite_node_set_block_size(f->node, 511);
	munit_assert_int(rv, ==, DQLITE_ERROR);
	rv = dqlite_node_set_block_size(f->node, 1024 * 512);
	munit_assert_int(rv, ==, DQLITE_ERROR);
	rv = dqlite_node_set_block_size(f->node, 64 * 1024);
	munit_assert_int(rv, ==, 0);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, blockSizeRunning, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_block_size(f->node, 64 * 1024);
	munit_assert_int(rv, ==, DQLITE_MISUSE);

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	return MUNIT_OK;
}

/* Our file locking prevents starting a second dqlite instance that
 * uses the same directory as a running instance. */
TEST(node, locked, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	int rv;

	dqlite_node *node2;
	rv = dqlite_node_create(2, "2", f->dir, &node2);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_bind_address(node2, "@456");
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s/dqlite-lock", f->dir);
	rv = access(buf, F_OK);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_start(node2);
	munit_assert_int(rv, ==, DQLITE_ERROR);
	munit_assert_string_equal(dqlite_node_errmsg(node2),
		"couldn't lock the raft directory");

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_start(node2);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_stop(node2);
	munit_assert_int(rv, ==, 0);
	dqlite_node_destroy(node2);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * dqlite_node_recover
 *
 ******************************************************************************/
TEST(node, recover, setUpForRecovery, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	/* Setup the infos structs */
	static struct dqlite_node_info infos[2] = {0};
	infos[0].id = 1;
	infos[0].address = "1";
	infos[1].id = 2;
	infos[1].address = "2";

	rv = dqlite_node_recover(f->node, infos, 2);
	munit_assert_int(rv, ==, 0);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, recoverExt, setUpForRecovery, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	/* Setup the infos structs */
	static struct dqlite_node_info_ext infos[2] = {0};
	infos[0].size = sizeof(*infos);
	infos[0].id = dqlite_generate_node_id("1");
	infos[0].address = PTR_TO_UINT64("1");
	infos[0].dqlite_role = DQLITE_VOTER;
	infos[1].size = sizeof(*infos);
	infos[1].id = dqlite_generate_node_id("2");
	;
	infos[1].address = PTR_TO_UINT64("2");
	infos[1].dqlite_role = DQLITE_SPARE;

	rv = dqlite_node_recover_ext(f->node, infos, 2);
	munit_assert_int(rv, ==, 0);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, recoverExtUnaligned, setUpForRecovery, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	/* Setup the infos structs */
	static struct dqlite_node_info_ext infos[1] = {0};
	infos[0].size = sizeof(*infos) + 1; /* Unaligned */
	infos[0].id = 1;
	infos[0].address = PTR_TO_UINT64("1");
	infos[0].dqlite_role = DQLITE_VOTER;

	rv = dqlite_node_recover_ext(f->node, infos, 1);
	munit_assert_int(rv, ==, DQLITE_MISUSE);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node, recoverExtTooSmall, setUpForRecovery, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	/* Setup the infos structs */
	static struct dqlite_node_info_ext infos[1] = {0};
	infos[0].size = DQLITE_NODE_INFO_EXT_SZ_ORIG - 1;
	infos[0].id = 1;
	infos[0].address = PTR_TO_UINT64("1");
	infos[0].dqlite_role = DQLITE_VOTER;

	rv = dqlite_node_recover_ext(f->node, infos, 1);
	munit_assert_int(rv, ==, DQLITE_MISUSE);

	startStopNode(f);
	return MUNIT_OK;
}

struct dqlite_node_info_ext_new
{
	struct dqlite_node_info_ext orig;
	uint64_t new1;
	uint64_t new2;
};

TEST(node, recoverExtNewFields, setUpForRecovery, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	/* Setup the infos structs */
	static struct dqlite_node_info_ext_new infos[1] = {0};
	infos[0].orig.size = sizeof(*infos);
	infos[0].orig.id = 1;
	infos[0].orig.address = PTR_TO_UINT64("1");
	infos[0].orig.dqlite_role = DQLITE_VOTER;
	infos[0].new1 = 0;
	infos[0].new2 = 0;

	rv = dqlite_node_recover_ext(f->node,
				     (struct dqlite_node_info_ext *)infos, 1);
	munit_assert_int(rv, ==, 0);

	startStopNode(f);
	return MUNIT_OK;
}

TEST(node,
     recoverExtNewFieldsNotZero,
     setUpForRecovery,
     tearDown,
     0,
     node_params)
{
	struct fixture *f = data;
	int rv;

	/* Setup the infos structs */
	static struct dqlite_node_info_ext_new infos[1] = {0};
	infos[0].orig.size = sizeof(*infos);
	infos[0].orig.id = 1;
	infos[0].orig.address = PTR_TO_UINT64("1");
	infos[0].orig.dqlite_role = DQLITE_VOTER;
	infos[0].new1 = 0;
	infos[0].new2 = 1; /* This will cause a failure */

	rv = dqlite_node_recover_ext(f->node,
				     (struct dqlite_node_info_ext *)infos, 1);
	munit_assert_int(rv, ==, DQLITE_MISUSE);

	startStopNode(f);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * dqlite_node_errmsg
 *
 ******************************************************************************/

TEST(node, errMsgNodeNull, NULL, NULL, 0, NULL)
{
	munit_assert_string_equal(dqlite_node_errmsg(NULL), "node is NULL");
	return MUNIT_OK;
}

TEST(node, errMsg, setUp, tearDown, 0, node_params)
{
	struct fixture *f = data;
	int rv;

	munit_assert_string_equal(dqlite_node_errmsg(f->node), "");

	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	return MUNIT_OK;
}
