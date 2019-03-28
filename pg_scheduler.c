#include <postgres.h>
#include <fmgr.h>

#include <access/xact.h>
#include <catalog/pg_type.h>
#include <commands/async.h>
#include <executor/spi.h>
#include <miscadmin.h>
#include <pgstat.h>
#include <postmaster/bgworker.h>
#include <storage/ipc.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/snapmgr.h>
#include <utils/timeout.h>
#include "utils/varlena.h"

typedef void (*Callback) (const char *src, va_list args);

PG_MODULE_MAGIC;

void _PG_init(void);

void loop(Datum arg);
void tick(Datum arg);
void task(Datum arg);

static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

static char *databases;

int period;
char *database;
char *username;
char *schema;
char *table;

static inline void sighup(SIGNAL_ARGS) {
    int save_errno = errno;
    got_sighup = true;
    (void)SetLatch(MyLatch);
    errno = save_errno;
}

static inline void sigterm(SIGNAL_ARGS) {
    int save_errno = errno;
    got_sigterm = true;
    (void)SetLatch(MyLatch);
    errno = save_errno;
}

static inline void launch_loop() {
    BackgroundWorker worker;
    MemSet(&worker, 0, sizeof(BackgroundWorker));
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_notify_pid = 0;
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
    if (snprintf(worker.bgw_library_name, sizeof("pg_scheduler"), "pg_scheduler") != sizeof("pg_scheduler") - 1) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    if (snprintf(worker.bgw_function_name, sizeof("loop"), "loop") != sizeof("loop") - 1) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    if (snprintf(worker.bgw_type, sizeof("pg_scheduler loop"), "pg_scheduler loop") != sizeof("pg_scheduler loop") - 1) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    if (snprintf(worker.bgw_name, sizeof("postgres postgres pg_scheduler loop"), "postgres postgres pg_scheduler loop") != sizeof("postgres postgres pg_scheduler loop") - 1) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    (void)RegisterBackgroundWorker(&worker);
}

void _PG_init(void) {
    if (IsBinaryUpgrade) return;
    if (!process_shared_preload_libraries_in_progress) ereport(FATAL, (errmsg("pg_scheduler can only be loaded via shared_preload_libraries"), errhint("Add pg_scheduler to the shared_preload_libraries configuration variable in postgresql.conf.")));
    (void)DefineCustomStringVariable("pg_scheduler.database", "pg_scheduler database", NULL, &databases, NULL, PGC_SIGHUP, 0, NULL, NULL, NULL);
    (void)launch_loop();
}

static inline void launch_tick(const char *database, const char *username) {
    int len, len2;
    pid_t pid;
    BackgroundWorkerHandle *handle;
    BackgroundWorker worker;
    MemSet(&worker, 0, sizeof(BackgroundWorker));
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_notify_pid = MyProcPid;
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
    if (snprintf(worker.bgw_library_name, sizeof("pg_scheduler"), "pg_scheduler") != sizeof("pg_scheduler") - 1) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    if (snprintf(worker.bgw_function_name, sizeof("tick"), "tick") != sizeof("tick") - 1) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    if (snprintf(worker.bgw_type, sizeof("pg_scheduler tick"), "pg_scheduler tick") != sizeof("pg_scheduler tick") - 1) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    len = (sizeof("%s %s pg_scheduler tick") - 1) + (strlen(database) - 1) + (strlen(username) - 1) - 1 - 1;
    if (snprintf(worker.bgw_name, len + 1, "%s %s pg_scheduler tick", database, username) != len) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    len = (sizeof("%s") - 1) + (strlen(database) - 1) - 1;
    if (snprintf(worker.bgw_extra, len + 1, "%s", database) != len) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    len2 = (sizeof("%s") - 1) + (strlen(username) - 1) - 1;
    if (snprintf(worker.bgw_extra + len + 1, len2 + 1, "%s", username) != len2) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    if (!RegisterDynamicBackgroundWorker(&worker, &handle)) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not register background process"), errhint("You may need to increase max_worker_processes.")));
    if (!handle) ereport(ERROR, (errmsg("!handle")));
    switch (WaitForBackgroundWorkerStartup(handle, &pid)) {
        case BGWH_STARTED: break;
        case BGWH_STOPPED: ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not start background process"), errhint("More details may be available in the server log.")));
        case BGWH_POSTMASTER_DIED: ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("cannot start background processes without postmaster"), errhint("Kill all remaining database processes and restart the database.")));
        default: ereport(ERROR, (errmsg("Unexpected bgworker handle status")));
    }
    (void)pfree(handle);
}

static inline void SPI_connect_execute_finish(const char *src, int timeout, Callback callback, ...) {
    (void)pgstat_report_activity(STATE_RUNNING, src);
    if (SPI_connect_ext(SPI_OPT_NONATOMIC) != SPI_OK_CONNECT) ereport(ERROR, (errmsg("SPI_connect_ext != SPI_OK_CONNECT")));
    (void)SPI_start_transaction();
    if (timeout > 0) (void)enable_timeout_after(STATEMENT_TIMEOUT, timeout); else (void)disable_timeout(STATEMENT_TIMEOUT, false);
//    elog(LOG, "SPI_connect_execute_finish src=\n%s", src);
    {
        va_list args;
        va_start(args, callback);
        (void)callback(src, args);
        va_end(args);
    }
    (void)disable_timeout(STATEMENT_TIMEOUT, false);
    if (SPI_finish() != SPI_OK_FINISH) ereport(ERROR, (errmsg("SPI_finish != SPI_OK_FINISH")));
    (void)ProcessCompletedNotifies();
    (void)pgstat_report_activity(STATE_IDLE, src);
    (void)pgstat_report_stat(true);
}

static inline void check_callback(const char *src, va_list args) {
    int nargs = va_arg(args, int);
    Oid *argtypes = va_arg(args, Oid *);
    Datum *Values = va_arg(args, Datum *);
    const char *Nulls = va_arg(args, const char *);
    if (SPI_execute_with_args(src, nargs, argtypes, Values, Nulls, false, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("SPI_execute_with_args != SPI_OK_SELECT")));
    (void)SPI_commit();
    for (uint64 row = 0; row < SPI_processed; row++) {
        bool isnull, start;
        char *username, *database = DatumGetCString(SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, SPI_fnumber(SPI_tuptable->tupdesc, "datname"), &isnull));
        if (isnull) ereport(ERROR, (errmsg("isnull")));
        username = DatumGetCString(SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, SPI_fnumber(SPI_tuptable->tupdesc, "usename"), &isnull));
        if (isnull) ereport(ERROR, (errmsg("isnull")));
        start = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, SPI_fnumber(SPI_tuptable->tupdesc, "start"), &isnull));
        if (isnull) ereport(ERROR, (errmsg("isnull")));
        elog(LOG, "check_callback row=%lu, database=%s, username=%s, start=%s", row, database, username, start?"true":"false");
        if (start) (void)launch_tick(database, username);
    }
}

static inline void check() {
    int i = 0;
    List *elemlist;
    StringInfoData buf;
    Oid *argtypes = NULL;
    Datum *Values = NULL;
    char *Nulls = NULL;
    char **str = NULL;
    elog(LOG, "check database=%s", databases);
    (void)initStringInfo(&buf);
    (void)appendStringInfoString(&buf,
        "WITH s AS (\n"
        "    SELECT      d.oid, d.datname, u.usesysid, u.usename\n"
        "    FROM        pg_database AS d\n"
        "    JOIN        pg_user AS u ON TRUE\n"
        "    INNER JOIN  pg_user AS i ON d.datdba = i.usesysid\n"
        "    WHERE       NOT datistemplate\n"
        "    AND         datallowconn\n");
    if (!databases) (void)appendStringInfoString(&buf, "    AND         i.usesysid = u.usesysid\n"); else {
        char *rawstring;
        if (!(rawstring = pstrdup(databases))) ereport(ERROR, (errmsg("!rawstring")));
        if (!SplitGUCList(rawstring, ',', &elemlist)) ereport(LOG, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("invalid list syntax in parameter \"pg_scheduler.database\" in postgresql.conf")));
        if (!elemlist) ereport(ERROR, (errmsg("!elemlist")));
        if (!(argtypes = palloc(sizeof(Oid) * list_length(elemlist) * 2))) ereport(ERROR, (errmsg("!argtypes")));
        if (!(Values = palloc(sizeof(Datum) * list_length(elemlist) * 2))) ereport(ERROR, (errmsg("!Values")));
        if (!(Nulls = palloc(sizeof(char) * list_length(elemlist) * 2))) ereport(ERROR, (errmsg("!Nulls")));
        if (!(str = palloc(sizeof(char *) * list_length(elemlist) * 2))) ereport(ERROR, (errmsg("!str")));
        (void)appendStringInfoString(&buf, "    AND         (d.datname, u.usename) IN (\n        ");
        for (ListCell *cell = list_head(elemlist); cell; cell = lnext(cell)) {
            const char *database_username = (const char *)lfirst(cell);
            char *rawstring;
            List *elemlist;
            if (!(rawstring = pstrdup(database_username))) ereport(ERROR, (errmsg("!rawstring")));
            if (!SplitIdentifierString(rawstring, ':', &elemlist)) ereport(LOG, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("invalid list syntax in parameter \"pg_scheduler.database\" in postgresql.conf")));
            if (!elemlist) ereport(ERROR, (errmsg("!elemlist"))); else {
                ListCell *cell = list_head(elemlist);
                const char *database = (const char *)lfirst(cell);
                const char *username = database;
                Nulls[2 * i] = ' ';
                Nulls[2 * i + 1] = ' ';
                if ((cell = lnext(cell))) username = (const char *)lfirst(cell);
                else Nulls[2 * i + 1] = 'n';
                elog(LOG, "check database=%s, username=%s", database, username);
                if (i > 0) (void)appendStringInfoString(&buf, ", ");
                (void)appendStringInfo(&buf, "($%i, COALESCE($%i, i.usename))", 2 * i + 1, 2 * i + 1 + 1);
                argtypes[2 * i] = TEXTOID;
                argtypes[2 * i + 1] = TEXTOID;
                if (!(str[2 * i] = pstrdup(database))) ereport(ERROR, (errmsg("!str")));
                if (!(str[2 * i + 1] = pstrdup(username))) ereport(ERROR, (errmsg("!str")));
                Values[2 * i] = CStringGetTextDatum(str[2 * i]);
                Values[2 * i + 1] = CStringGetTextDatum(str[2 * i + 1]);
            }
            (void)pfree(rawstring);
            (void)list_free(elemlist);
            i++;
        }
        (void)appendStringInfoString(&buf, "\n    )\n");
        (void)pfree(rawstring);
        (void)list_free(elemlist);
    }
    (void)appendStringInfoString(&buf,
        "), l AS (\n"
        "    SELECT * FROM pg_locks WHERE locktype = 'advisory' AND mode = 'ExclusiveLock' AND granted\n"
        ")\n"
        "SELECT      datname, usename, TRUE AS start\n"
        "FROM        s\n"
        "WHERE       NOT EXISTS (SELECT pid FROM l WHERE classid = oid AND objid = usesysid AND database = oid)\n"
        "UNION\n"
        "SELECT      datname, usename, NOT pg_terminate_backend(pid) AS start\n"
        "FROM        pg_stat_activity\n"
        "INNER JOIN  l USING (pid)\n"
        "WHERE       (datname, usename) NOT IN (SELECT datname, usename FROM s)\n"
        "AND         classid = datid AND objid = usesysid AND database = datid");
    (void)SPI_connect_execute_finish(buf.data, StatementTimeout, check_callback, i * 2, argtypes, Values, Nulls);
    (void)pfree(buf.data);
    (void)pfree(argtypes);
    (void)pfree(Values);
    (void)pfree(Nulls);
    for (int j = 0; j < i * 2; j++) (void)pfree(str[j]);
    (void)pfree(str);
}

void loop(Datum arg) {
    elog(LOG, "loop database=%s", databases);
    (pqsigfunc)pqsignal(SIGHUP, sighup);
    (pqsigfunc)pqsignal(SIGTERM, sigterm);
    (void)BackgroundWorkerUnblockSignals();
    (void)BackgroundWorkerInitializeConnection("postgres", "postgres", 0);
    (void)check();
    do {
        int rc = WaitLatch(MyLatch, WL_LATCH_SET | /*WL_TIMEOUT |*/ WL_POSTMASTER_DEATH, LONG_MAX, PG_WAIT_EXTENSION);
        if (rc & WL_LATCH_SET) elog(LOG, "loop WL_LATCH_SET");
        //if (rc & WL_TIMEOUT) elog(LOG, "loop WL_TIMEOUT");
        if (rc & WL_POSTMASTER_DEATH) elog(LOG, "loop WL_POSTMASTER_DEATH");
        if (got_sigterm) elog(LOG, "loop got_sigterm");
        if (got_sighup) elog(LOG, "loop got_sighup");
//        if (ProcDiePending) elog(LOG, "loop ProcDiePending");
        if (rc & WL_POSTMASTER_DEATH) (void)proc_exit(1);
        if (rc & WL_LATCH_SET) {
            (void)ResetLatch(MyLatch);
            CHECK_FOR_INTERRUPTS();
        }
        if (got_sighup) {
            got_sighup = false;
            (void)ProcessConfigFile(PGC_SIGHUP);
            (void)check();
        }
        if (got_sigterm) (void)proc_exit(0);
    } while (!got_sigterm);
    (void)proc_exit(0);
}

static inline void lock_callback(const char *src, va_list args) {
    if (SPI_execute(src, false, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("SPI_execute != SPI_OK_SELECT")));
    (void)SPI_commit();
    if (SPI_processed != 1) ereport(ERROR, (errmsg("SPI_processed != 1"))); else {
        bool isnull;
        bool lock = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, SPI_fnumber(SPI_tuptable->tupdesc, "pg_try_advisory_lock"), &isnull));
        if (isnull) ereport(ERROR, (errmsg("isnull")));
        if (!lock) ereport(ERROR, (errmsg("Already running database=%s, username=%s", database, username)));
    }
}

static inline void lock() {
    const char *src = "SELECT pg_try_advisory_lock(pg_database.oid::INT, pg_user.usesysid::INT) FROM pg_database, pg_user WHERE datname = current_catalog AND usename = current_user";
    (void)SPI_connect_execute_finish(src, StatementTimeout, lock_callback);
}

static inline void schema_callback(const char *src, va_list args) {
    if (SPI_execute(src, false, 0) != SPI_OK_UTILITY) ereport(ERROR, (errmsg("SPI_execute != SPI_OK_UTILITY")));
    (void)SPI_commit();
}

static inline void init_schema() {
    StringInfoData buf;
    elog(LOG, "init_schema database=%s, username=%s, period=%i, schema=%s, table=%s", database, username, period, schema, table);
    (void)initStringInfo(&buf);
    (void)appendStringInfo(&buf, "CREATE SCHEMA IF NOT EXISTS %s", quote_identifier(schema));
    (void)SPI_connect_execute_finish(buf.data, StatementTimeout, schema_callback);
    if (buf.data != NULL) (void)pfree(buf.data);
}

static inline void table_callback(const char *src, va_list args) {
    if (SPI_execute(src, false, 0) != SPI_OK_UTILITY) ereport(ERROR, (errmsg("SPI_execute != SPI_OK_UTILITY")));
    (void)SPI_commit();
}

static inline void init_table() {
    StringInfoData buf;
    elog(LOG, "init_table database=%s, username=%s, period=%i, schema=%s, table=%s", database, username, period, schema, table);
    (void)initStringInfo(&buf);
    (void)appendStringInfoString(&buf, "CREATE TABLE IF NOT EXISTS ");
    if (schema != NULL) (void)appendStringInfo(&buf, "%s.", quote_identifier(schema));
    (void)appendStringInfo(&buf, "%s (\n", quote_identifier(table));
    (void)appendStringInfo(&buf,
        "    id BIGSERIAL NOT NULL PRIMARY KEY,\n"
        "    dt TIMESTAMP NOT NULL DEFAULT NOW(),\n"
        "    start TIMESTAMP,\n"
        "    stop TIMESTAMP,\n"
        "    queue TEXT NOT NULL DEFAULT 'default',\n"
        "    max INT,\n"
        "    request TEXT NOT NULL,\n"
        "    response TEXT,\n"
        "    state TEXT NOT NULL DEFAULT 'QUEUE',\n"
        "    timeout INTERVAL"
        ")");
    (void)SPI_connect_execute_finish(buf.data, StatementTimeout, table_callback);
    if (buf.data != NULL) (void)pfree(buf.data);
}

static inline void index_callback(const char *src, va_list args) {
    if (SPI_execute(src, false, 0) != SPI_OK_UTILITY) ereport(ERROR, (errmsg("SPI_execute != SPI_OK_UTILITY")));
    (void)SPI_commit();
}

static inline void init_index(const char *index) {
    StringInfoData buf, name;
    elog(LOG, "init_index database=%s, username=%s, period=%i, schema=%s, table=%s, index=%s", database, username, period, schema, table, index);
    (void)initStringInfo(&buf);
    (void)initStringInfo(&name);
    (void)appendStringInfo(&name, "%s_%s_idx", table, index);
    (void)appendStringInfo(&buf, "CREATE INDEX IF NOT EXISTS %s ON ", quote_identifier(name.data));
    if (schema != NULL) (void)appendStringInfo(&buf, "%s.", quote_identifier(schema));
    (void)appendStringInfo(&buf, "%s USING btree (%s)", quote_identifier(table), quote_identifier(index));
    (void)SPI_connect_execute_finish(buf.data, StatementTimeout, index_callback);
    if (buf.data != NULL) (void)pfree(buf.data);
    if (name.data != NULL) (void)pfree(name.data);
}

static inline void launch_task(Datum arg, const char *queue) {
    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;
    pid_t pid;
    int len, len2, len3, len4;
    uint64 id = DatumGetInt64(arg);
    elog(LOG, "launch_task database=%s, username=%s, schema=%s, table=%s, id=%lu, queue=%s", database, username, schema, table, id, queue);
    MemSet(&worker, 0, sizeof(BackgroundWorker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    worker.bgw_notify_pid = MyProcPid;
    worker.bgw_main_arg = arg;
    if (snprintf(worker.bgw_library_name, sizeof("pg_scheduler"), "pg_scheduler") != sizeof("pg_scheduler") - 1) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    if (snprintf(worker.bgw_function_name, sizeof("task"), "task") != sizeof("task") - 1) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    len = (sizeof("pg_scheduler task %s") - 1) + (strlen(queue) - 1) - 1;
    if (snprintf(worker.bgw_type, len + 1, "pg_scheduler task %s", queue) != len) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    len = (sizeof("%s %s pg_scheduler task %s %lu") - 1) + (strlen(database) - 1) + (strlen(username) - 1) + (strlen(queue) - 1) - 1 - 1 - 1 - 2;
    for (int number = id; number /= 10; len++);
    if (snprintf(worker.bgw_name, len + 1, "%s %s pg_scheduler task %s %lu", database, username, queue, id) != len) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    len = (sizeof("%s") - 1) + (strlen(database) - 1) - 1;
    if (snprintf(worker.bgw_extra, len + 1, "%s", database) != len) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    len2 = (sizeof("%s") - 1) + (strlen(username) - 1) - 1;
    if (snprintf(worker.bgw_extra + len + 1, len2 + 1, "%s", username) != len2) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    len3 = (sizeof("%s") - 1) + (strlen(table) - 1) - 1;
    if (snprintf(worker.bgw_extra + len + 1 + len2 + 1, len3 + 1, "%s", table) != len3) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    if (schema != NULL) {
        len4 = (sizeof("%s") - 1) + (strlen(schema) - 1) - 1;
        if (snprintf(worker.bgw_extra + len + 1 + len2 + 1 + len3 + 1, len4 + 1, "%s", schema) != len4) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("snprintf")));
    }
    if (!RegisterDynamicBackgroundWorker(&worker, &handle)) ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not register background process"), errhint("You may need to increase max_worker_processes.")));
    switch (WaitForBackgroundWorkerStartup(handle, &pid)) {
        case BGWH_STARTED: break;
        case BGWH_STOPPED: ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not start background process"), errhint("More details may be available in the server log.")));
        case BGWH_POSTMASTER_DIED: ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("cannot start background processes without postmaster"), errhint("Kill all remaining database processes and restart the database.")));
        default: ereport(ERROR, (errmsg("Unexpected bgworker handle status")));
    }
    if (handle != NULL) (void)pfree(handle);
}

static inline void assign_callback(const char *src, va_list args) {
    if (SPI_execute(src, false, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("SPI_execute != SPI_OK_SELECT")));
    (void)SPI_commit();
    for (uint64 row = 0; row < SPI_processed; row++) {
        bool isnull;
        char *queue;
        Datum id = SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, SPI_fnumber(SPI_tuptable->tupdesc, "id"), &isnull);
        if (isnull) ereport(ERROR, (errmsg("isnull")));
        queue = TextDatumGetCString(SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, SPI_fnumber(SPI_tuptable->tupdesc, "queue"), &isnull));
        if (isnull) ereport(ERROR, (errmsg("isnull")));
        elog(LOG, "assign_callback row=%lu, id=%lu, queue=%s", row, DatumGetInt64(id), queue);
        (void)launch_task(id, queue);
        if (queue != NULL) (void)pfree(queue);
    }
}

static inline void assign() {
    StringInfoData buf;
    (void)initStringInfo(&buf);
    (void)appendStringInfoString(&buf,
        "WITH s AS (\n"
        "    SELECT      id, queue, COALESCE(max, ~(1<<31)) AS max\n"
        "    FROM        ");
    if (schema != NULL) (void)appendStringInfo(&buf, "%s.", quote_identifier(schema));
    (void)appendStringInfo(&buf, "%s\n"
        "    WHERE       state = 'QUEUE'\n"
        "    AND         dt <= now()\n"
        "    AND         COALESCE(max, ~(1<<31)) > (SELECT count(pid) FROM pg_stat_activity WHERE datname = current_catalog AND usename = current_user AND backend_type = 'pg_scheduler task '||queue)\n"
        "    ORDER BY    max DESC, id\n"
        ") SELECT id, queue FROM s LIMIT (SELECT max FROM s LIMIT 1)", quote_identifier(table));
    (void)SPI_connect_execute_finish(buf.data, StatementTimeout, assign_callback);
    if (buf.data != NULL) (void)pfree(buf.data);
}

static inline void init() {
    if (schema != NULL) (void)init_schema();
    (void)init_table();
    (void)init_index("dt");
    (void)init_index("state");
}

void tick(Datum arg) {
    StringInfoData buf;
    database = MyBgworkerEntry->bgw_extra;
    username = database + strlen(database) + 1;
    (void)initStringInfo(&buf);
    (void)appendStringInfo(&buf, "pg_scheduler_period.%s", database);
    (void)DefineCustomIntVariable(buf.data, "how often to run tick", NULL, &period, 1000, 1, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);
    (void)resetStringInfo(&buf);
    (void)appendStringInfo(&buf, "pg_scheduler_schema.%s", database);
    (void)DefineCustomStringVariable(buf.data, "pg_scheduler schema", NULL, &schema, NULL, PGC_SIGHUP, 0, NULL, NULL, NULL);
    (void)resetStringInfo(&buf);
    (void)appendStringInfo(&buf, "pg_scheduler_table.%s", database);
    (void)DefineCustomStringVariable(buf.data, "pg_scheduler table", NULL, &table, "task", PGC_SIGHUP, 0, NULL, NULL, NULL);
    if (buf.data != NULL) (void)pfree(buf.data);
    elog(LOG, "tick database=%s, username=%s, period=%i, schema=%s, table=%s", database, username, period, schema, table);
    (pqsigfunc)pqsignal(SIGHUP, sighup);
    (pqsigfunc)pqsignal(SIGTERM, sigterm);
    (void)BackgroundWorkerUnblockSignals();
    (void)BackgroundWorkerInitializeConnection(database, username, 0);
    (void)lock();
    (void)init();
    do {
        int rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, period, PG_WAIT_EXTENSION);
        if (rc & WL_LATCH_SET) elog(LOG, "tick WL_LATCH_SET");
        //if (rc & WL_TIMEOUT) elog(LOG, "tick WL_TIMEOUT");
        if (rc & WL_POSTMASTER_DEATH) elog(LOG, "tick WL_POSTMASTER_DEATH");
        if (got_sigterm) elog(LOG, "tick got_sigterm");
        if (got_sighup) elog(LOG, "tick got_sighup");
//        if (ProcDiePending) elog(LOG, "loop ProcDiePending");
        if (rc & WL_POSTMASTER_DEATH) (void)proc_exit(1);
        if (rc & WL_LATCH_SET) {
            (void)ResetLatch(MyLatch);
            CHECK_FOR_INTERRUPTS();
        }
        if (got_sighup) {
            got_sighup = false;
            (void)ProcessConfigFile(PGC_SIGHUP);
            (void)init();
        }
        if (got_sigterm) (void)proc_exit(0);
        if (rc & WL_TIMEOUT) (void)assign();
    } while (!got_sigterm);
    (void)proc_exit(0);
}

static inline void work_callback(const char *src, va_list args) {
    int nargs = va_arg(args, int);
    Oid *argtypes = va_arg(args, Oid *);
    Datum *Values = va_arg(args, Datum *);
    const char *Nulls = va_arg(args, const char *);
    if (SPI_execute_with_args(src, nargs, argtypes, Values, Nulls, false, 0) != SPI_OK_UPDATE_RETURNING) ereport(ERROR, (errmsg("SPI_execute_with_args != SPI_OK_UPDATE_RETURNING")));
    (void)SPI_commit();
    if (SPI_processed != 1) ereport(ERROR, (errmsg("SPI_processed != 1"))); else {
        char **data = va_arg(args, char **);
        int *timeout = va_arg(args, int *);
        bool isnull;
        char *value = TextDatumGetCString(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, SPI_fnumber(SPI_tuptable->tupdesc, "request"), &isnull));
        if (isnull) ereport(ERROR, (errmsg("isnull")));
        *timeout = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, SPI_fnumber(SPI_tuptable->tupdesc, "timeout"), &isnull));
        if (isnull) ereport(ERROR, (errmsg("isnull")));
        *data = strdup(value);
        elog(LOG, "work timeout=%i, data=\n%s", *timeout, *data);
        if (value != NULL) (void)pfree(value);
    }
}

static inline void work(Datum arg, char **data, int *timeout) {
    Oid argtypes[] = {INT8OID};
    Datum Values[] = {arg};
    StringInfoData buf;
    elog(LOG, "work database=%s, username=%s, schema=%s, table=%s, id=%lu", database, username, schema, table, DatumGetInt64(arg));
    (void)initStringInfo(&buf);
    (void)appendStringInfoString(&buf, "UPDATE ");
    if (schema != NULL) (void)appendStringInfo(&buf, "%s.", quote_identifier(schema));
    (void)appendStringInfo(&buf, "%s SET state = 'WORK', start = now() WHERE id = $1 RETURNING request, COALESCE(EXTRACT(epoch FROM timeout), 0)::INT * 1000 AS timeout", quote_identifier(table));
    elog(LOG, "work buf.data=%s", buf.data);
    (void)SPI_connect_execute_finish(buf.data, StatementTimeout, work_callback, sizeof(argtypes)/sizeof(argtypes[0]), argtypes, Values, NULL, data, timeout);
    if (buf.data != NULL) (void)pfree(buf.data);
}

static inline void done_callback(const char *src, va_list args) {
    int nargs = va_arg(args, int);
    Oid *argtypes = va_arg(args, Oid *);
    Datum *Values = va_arg(args, Datum *);
    const char *Nulls = va_arg(args, const char *);
    if (SPI_execute_with_args(src, nargs, argtypes, Values, Nulls, false, 0) != SPI_OK_UPDATE) ereport(ERROR, (errmsg("SPI_execute_with_args != SPI_OK_UPDATE")));
    (void)SPI_commit();
}

static inline void done(Datum arg, const char *data, const char *state) {
    Oid argtypes[] = {TEXTOID, TEXTOID, INT8OID};
    Datum Values[] = {CStringGetTextDatum(state), CStringGetTextDatum(data!=NULL?data:"(null)"), arg};
    StringInfoData buf;
    (void)initStringInfo(&buf);
    (void)appendStringInfoString(&buf, "UPDATE ");
    if (schema != NULL) (void)appendStringInfo(&buf, "%s.", quote_identifier(schema));
    (void)appendStringInfo(&buf, "%s SET state = $1, stop = now(), response=$2 WHERE id = $3", quote_identifier(table));
    elog(LOG, "done buf.data=%s", buf.data);
    (void)SPI_connect_execute_finish(buf.data, StatementTimeout, done_callback, sizeof(argtypes)/sizeof(argtypes[0]), argtypes, Values, NULL);
    if (buf.data != NULL) (void)pfree(buf.data);
}

static inline void success(char **data, char **state) {
    StringInfoData buf;
    (void)initStringInfo(&buf);
    if ((SPI_tuptable != NULL) && (SPI_processed > 0)) {
        for (int col = 1; col <= SPI_tuptable->tupdesc->natts; col++) {
            char *name = SPI_fname(SPI_tuptable->tupdesc, col);
            char *type = SPI_gettype(SPI_tuptable->tupdesc, col);
            (void)appendStringInfo(&buf, "%s::%s", name, type);
            if (col > 1) (void)appendStringInfoString(&buf, "\t");
            if (name != NULL) (void)pfree(name);
            if (type != NULL) (void)pfree(type);
        }
        (void)appendStringInfoString(&buf, "\n");
        for (uint64 row = 0; row < SPI_processed; row++) {
            for (int col = 1; col <= SPI_tuptable->tupdesc->natts; col++) {
                char *value = SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, col);
                (void)appendStringInfo(&buf, "%s", value);
                if (col > 1) (void)appendStringInfoString(&buf, "\t");
                if (value != NULL) (void)pfree(value);
            }
            if (row < SPI_processed - 1) (void)appendStringInfoString(&buf, "\n");
        }
        elog(LOG, "success\n%s", buf.data);
    }
    *data = buf.data;
    *state = "DONE";
}

static inline void error(char **data, char **state) {
    ErrorData *edata = CopyErrorData();
    StringInfoData buf;
    (void)initStringInfo(&buf);
    (void)appendStringInfo(&buf,
        "elevel::int4\t%i\n"
        "output_to_server::bool\t%s\n"
        "output_to_client::bool\t%s\n"
        "show_funcname::bool\t%s\n"
        "hide_stmt::bool\t%s\n"
        "hide_ctx::bool\t%s\n"
        "filename::text\t%s\n"
        "lineno::int4\t%i\n"
        "funcname::text\t%s\n"
        "domain::text\t%s\n"
        "context_domain::text\t%s\n"
        "sqlerrcode::int4\t%i\n"
        "message::text\t%s\n"
        "detail::text\t%s\n"
        "detail_log::text\t%s\n"
        "hint::text\t%s\n"
        "context::text\t%s\n"
        "message_id::text\t%s\n"
        "schema_name::text\t%s\n"
        "table_name::text\t%s\n"
        "column_name::text\t%s\n"
        "datatype_name::text\t%s\n"
        "constraint_name::text\t%s\n"
        "cursorpos::int4\t%i\n"
        "internalpos::int4\t%i\n"
        "internalquery::text\t%s\n"
        "saved_errno::int4\t%i",
        edata->elevel,
        edata->output_to_server?"true":"false",
        edata->output_to_client?"true":"false",
        edata->show_funcname?"true":"false",
        edata->hide_stmt?"true":"false",
        edata->hide_ctx?"true":"false",
        edata->filename,
        edata->lineno,
        edata->funcname,
        edata->domain,
        edata->context_domain,
        edata->sqlerrcode,
        edata->message,
        edata->detail,
        edata->detail_log,
        edata->hint,
        edata->context,
        edata->message_id,
        edata->schema_name,
        edata->table_name,
        edata->column_name,
        edata->datatype_name,
        edata->constraint_name,
        edata->cursorpos,
        edata->internalpos,
        edata->internalquery,
        edata->saved_errno
    );
    (void)FreeErrorData(edata);
    elog(LOG, "error\n%s", buf.data);
    *data = buf.data;
    *state = "FAIL";
}

static inline void execute_callback(const char *src, va_list args) {
    PG_TRY(); {
        if (SPI_execute(src, false, 0) < 0) ereport(ERROR, (errmsg("SPI_execute < 0"))); else {
            char **data = va_arg(args, char **);
            char **state = va_arg(args, char **);
            (void)success(data, state);
            (void)SPI_commit();
        }
    } PG_CATCH(); {
        char **data = va_arg(args, char **);
        char **state = va_arg(args, char **);
        (void)error(data, state);
        (void)SPI_rollback();
    } PG_END_TRY();
}

static inline void execute(Datum arg) {
    char *src = NULL;
    char *data = NULL;
    char *state;
    int timeout = 0;
    (void)work(arg, &src, &timeout);
    if ((StatementTimeout > 0) && (StatementTimeout < timeout)) timeout = StatementTimeout;
//    elog(LOG, "execute src=%s", src);
    elog(LOG, "execute database=%s, username=%s, schema=%s, table=%s, timeout=%i, src=\n%s", database, username, schema, table, timeout, src);
    (void)SPI_connect_execute_finish(src, timeout, execute_callback, &data, &state);
//    elog(LOG, "src=%s", src);
    (void)done(arg, data, state);
    if (src != NULL) (void)free(src);
    if (data != NULL) (void)pfree(data);
}

void task(Datum arg) {
    database = MyBgworkerEntry->bgw_extra;
    username = database + strlen(database) + 1;
    table = username + strlen(username) + 1;
    schema = table + strlen(table) + 1;
    if (strlen(schema) == 0) schema = NULL;
    elog(LOG, "task database=%s, username=%s, schema=%s, table=%s, id=%lu", database, username, schema, table, DatumGetInt64(arg));
    (void)BackgroundWorkerUnblockSignals();
    (void)BackgroundWorkerInitializeConnection(database, username, 0);
    (void)execute(arg);
}