#include "include.h"

extern const char *null;
extern volatile sig_atomic_t sighup;
extern volatile sig_atomic_t sigterm;

static void tick_schema(Work *work) {
    StringInfoData buf;
    List *names;
    const char *schema_quote = quote_identifier(work->schema);
    D1("user = %s, data = %s, schema = %s, table = %s", work->user, work->data, work->schema, work->table);
    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE SCHEMA %s", schema_quote);
    names = stringToQualifiedNameList(schema_quote);
    SPI_connect_my(buf.data);
    if (!OidIsValid(get_namespace_oid(strVal(linitial(names)), true))) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    SPI_commit_my();
    SPI_finish_my();
    list_free_deep(names);
    if (schema_quote != work->schema) pfree((void *)schema_quote);
    pfree(buf.data);
    SetConfigOptionMy("pg_task.schema", work->schema);
}

static void tick_type(Work *work) {
    StringInfoData buf, name;
    Oid type = InvalidOid;
    int32 typmod;
    const char *schema_quote = work->schema ? quote_identifier(work->schema) : NULL;
    D1("user = %s, data = %s, schema = %s, table = %s", work->user, work->data, work->schema ? work->schema : null, work->table);
    initStringInfo(&name);
    if (schema_quote) appendStringInfo(&name, "%s.", schema_quote);
    appendStringInfoString(&name, "state");
    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE TYPE %s AS ENUM ('PLAN', 'TAKE', 'WORK', 'DONE', 'FAIL', 'STOP')", name.data);
    SPI_connect_my(buf.data);
    parseTypeString(name.data, &type, &typmod, true);
    if (!OidIsValid(type)) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    SPI_commit_my();
    SPI_finish_my();
    if (work->schema && schema_quote && work->schema != schema_quote) pfree((void *)schema_quote);
    pfree(name.data);
    pfree(buf.data);
}

static bool tick_table(Work *work) {
    StringInfoData buf, name;
    List *names;
    const RangeVar *relation;
    const char *name_quote;
    D1("user = %s, data = %s, schema = %s, table = %s, schema_table = %s", work->user, work->data, work->schema ? work->schema : null, work->table, work->schema_table);
    if (work->oid) pg_advisory_unlock_int8_my(work->oid);
    SetConfigOptionMy("pg_task.table", work->table);
    initStringInfo(&name);
    appendStringInfo(&name, "%s_parent_fkey", work->table);
    name_quote = quote_identifier(name.data);
    initStringInfo(&buf);
    appendStringInfo(&buf,
        "CREATE TABLE %1$s (\n"
        "    id bigserial NOT NULL PRIMARY KEY,\n"
        "    parent int8 DEFAULT current_setting('pg_task.id', true)::int8,\n"
        "    dt timestamptz NOT NULL DEFAULT current_timestamp,\n"
        "    start timestamptz,\n"
        "    stop timestamptz,\n"
        "    \"group\" text NOT NULL DEFAULT 'group',\n"
        "    max int4,\n"
        "    pid int4,\n"
        "    request text NOT NULL,\n"
        "    response text,\n"
        "    state state NOT NULL DEFAULT 'PLAN'::state,\n"
        "    timeout interval,\n"
        "    delete boolean NOT NULL DEFAULT false,\n"
        "    repeat interval,\n"
        "    drift boolean NOT NULL DEFAULT true,\n"
        "    count int4,\n"
        "    live interval,\n"
        "    remote text,\n"
        "    append boolean NOT NULL DEFAULT false,\n"
        "    header boolean NOT NULL DEFAULT true,\n"
        "    string boolean NOT NULL DEFAULT true,\n"
        "    \"null\" text NOT NULL DEFAULT '\\N',\n"
        "    delimiter \"char\" NOT NULL DEFAULT '\t',\n"
        "    quote \"char\",\n"
        "    escape \"char\",\n"
        "    CONSTRAINT %2$s FOREIGN KEY (parent) REFERENCES %1$s (id) MATCH SIMPLE ON UPDATE CASCADE ON DELETE SET NULL\n"
        ")", work->schema_table, name_quote);
    names = stringToQualifiedNameList(work->schema_table);
    relation = makeRangeVarFromNameList(names);
    SPI_connect_my(buf.data);
    if (!OidIsValid(RangeVarGetRelid(relation, NoLock, true))) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    work->oid = RangeVarGetRelid(relation, NoLock, false);
    SPI_commit_my();
    SPI_finish_my();
    pfree((void *)relation);
    list_free_deep(names);
    if (name_quote != name.data) pfree((void *)name_quote);
    pfree(name.data);
    SetConfigOptionMy("pg_task.table", work->table);
    resetStringInfo(&buf);
    appendStringInfo(&buf, "%i", work->oid);
    SetConfigOptionMy("pg_task.oid", buf.data);
    pfree(buf.data);
    if (!pg_try_advisory_lock_int8_my(work->oid)) { W("!pg_try_advisory_lock_int8_my(%i)", work->oid); return true; }
    return false;
}

static void tick_index(Work *work, const char *index) {
    StringInfoData buf, name;
    List *names;
    const RangeVar *relation;
    const char *name_quote;
    const char *index_quote = quote_identifier(index);
    D1("user = %s, data = %s, schema = %s, table = %s, index = %s, schema_table = %s", work->user, work->data, work->schema ? work->schema : null, work->table, index, work->schema_table);
    initStringInfo(&name);
    appendStringInfo(&name, "%s_%s_idx", work->table, index);
    name_quote = quote_identifier(name.data);
    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE INDEX %s ON %s USING btree (%s)", name_quote, work->schema_table, index_quote);
    names = stringToQualifiedNameList(name_quote);
    relation = makeRangeVarFromNameList(names);
    SPI_connect_my(buf.data);
    if (!OidIsValid(RangeVarGetRelid(relation, NoLock, true))) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    SPI_commit_my();
    SPI_finish_my();
    pfree((void *)relation);
    list_free_deep(names);
    pfree(buf.data);
    pfree(name.data);
    if (name_quote != name.data) pfree((void *)name_quote);
    if (index_quote != index) pfree((void *)index_quote);
}

static void tick_free(Task *task) {
    if (task->group) pfree(task->group);
    if (task->null) pfree(task->null);
    if (task->remote) pfree(task->remote);
    if (task->request) pfree(task->request);
    if (task->response.data) pfree(task->response.data);
    pfree(task);
}

static void tick_finish(Task *task) {
    queue_remove(&task->queue);
    PQfinish(task->conn);
    tick_free(task);
}

static void tick_error(Task *task, const char *msg) {
    char *err = PQerrorMessage(task->conn);
    int len = strlen(err);
    initStringInfo(&task->response);
    appendStringInfoString(&task->response, msg);
    if (len) appendStringInfo(&task->response, " and %.*s", len - 1, err);
    W(task->response.data);
    task->fail = true;
    task_done(task);
    tick_finish(task);
}

static void tick_remote(Work *work, const int64 id, char *group, char *remote, const int max) {
    const char **keywords;
    const char **values;
    StringInfoData buf, buf2;
    int arg = 3;
    char *err;
    char *options = NULL;
//    bool password = false;
    Task *task = MemoryContextAllocZero(TopMemoryContext, sizeof(*task));
    PQconninfoOption *opts = PQconninfoParse(remote, &err);
    task->group = group;
    task->id = id;
    task->max = max;
    task->remote = remote;
    task->work = work;
    D1("id = %li, group = %s, remote = %s, max = %i, oid = %i", task->id, task->group, task->remote ? task->remote : null, task->max, work->oid);
    if (!opts) {
        initStringInfo(&task->response);
        appendStringInfoString(&task->response, "!PQconninfoParse");
        if (err) {
            int len = strlen(err);
            if (len) appendStringInfo(&task->response, " and %.*s", len - 1, err);
            PQfreemem(err);
        }
        W(task->response.data);
        task->fail = true;
        task_done(task);
        tick_free(task);
        return;
    }
    for (PQconninfoOption *opt = opts; opt->keyword; opt++) {
        if (!opt->val) continue;
        D1("%s = %s", opt->keyword, opt->val);
//        if (!pg_strncasecmp(opt->keyword, "password", sizeof("password") - 1)) password = true;
        if (!pg_strncasecmp(opt->keyword, "fallback_application_name", sizeof("fallback_application_name") - 1)) continue;
        if (!pg_strncasecmp(opt->keyword, "application_name", sizeof("application_name") - 1)) continue;
        if (!pg_strncasecmp(opt->keyword, "options", sizeof("options") - 1)) { options = opt->val; continue; }
        arg++;
    }
/*    if (!superuser() && !password) {
        initStringInfo(&task->response);
        appendStringInfoString(&task->response, "!superuser && !password");
        W(task->response.data);
        task->fail = true;
        task_done(task);
        tick_free(task);
        return;
    }*/
    keywords = palloc(arg * sizeof(*keywords));
    values = palloc(arg * sizeof(*values));
    initStringInfo(&buf);
    appendStringInfo(&buf, "pg_task %s%s%s %s", work->schema ? work->schema : "", work->schema ? " " : "", work->table, group);
    arg = 0;
    keywords[arg] = "application_name";
    values[arg] = buf.data;
    initStringInfo(&buf2);
    if (options) appendStringInfoString(&buf2, options);
    appendStringInfo(&buf2, "%s-c pg_task.data=%s", buf2.len ? " " : "", work->data);
    appendStringInfo(&buf2, " -c pg_task.user=%s", work->user);
    if (work->schema) appendStringInfo(&buf2, " -c pg_task.schema=%s", work->schema);
    appendStringInfo(&buf2, " -c pg_task.table=%s", work->table);
    appendStringInfo(&buf2, " -c pg_task.oid=%i", work->oid);
    appendStringInfo(&buf2, " -c pg_task.group=%s", group);
    arg++;
    keywords[arg] = "options";
    values[arg] = buf2.data;
    for (PQconninfoOption *opt = opts; opt->keyword; opt++) {
        if (!opt->val) continue;
        if (!pg_strncasecmp(opt->keyword, "fallback_application_name", sizeof("fallback_application_name") - 1)) continue;
        if (!pg_strncasecmp(opt->keyword, "application_name", sizeof("application_name") - 1)) continue;
        if (!pg_strncasecmp(opt->keyword, "options", sizeof("options") - 1)) continue;
        arg++;
        keywords[arg] = opt->keyword;
        values[arg] = opt->val;
    }
    arg++;
    keywords[arg] = NULL;
    values[arg] = NULL;
    task->events = WL_SOCKET_WRITEABLE;
    task->start = GetCurrentTimestamp();
    queue_insert_tail(&work->queue, &task->queue);
    if (!(task->conn = PQconnectStartParams(keywords, values, false))) tick_error(task, "!PQconnectStartParams"); else
    if (PQstatus(task->conn) == CONNECTION_BAD) tick_error(task, "PQstatus == CONNECTION_BAD"); else
    if (!PQisnonblocking(task->conn) && PQsetnonblocking(task->conn, true) == -1) tick_error(task, "PQsetnonblocking == -1"); else
    if ((task->fd = PQsocket(task->conn)) < 0) tick_error(task, "PQsocket < 0"); else
//    if (!superuser() && PQconnectionNeedsPassword(task->conn) && !PQconnectionUsedPassword(task->conn)) tick_error(task, "!superuser && PQconnectionNeedsPassword && !PQconnectionUsedPassword"); else
    if (PQclientEncoding(task->conn) != GetDatabaseEncoding()) PQsetClientEncoding(task->conn, GetDatabaseEncodingName());
    pfree(buf.data);
    pfree(buf2.data);
    pfree(keywords);
    pfree(values);
    PQconninfoFree(opts);
}

static void tick_task(const Work *work, const int64 id, char *group, const int max) {
    StringInfoData buf;
    int user_len = strlen(work->user), data_len = strlen(work->data), schema_len = work->schema ? strlen(work->schema) : 0, table_len = strlen(work->table), group_len = strlen(group), max_len = sizeof(max), oid_len = sizeof(work->oid);
    BackgroundWorker worker;
    char *p = worker.bgw_extra;
    D1("user = %s, data = %s, schema = %s, table = %s, id = %li, group = %s, max = %i, oid = %i", work->user, work->data, work->schema ? work->schema : null, work->table, id, group, max, work->oid);
    MemSet(&worker, 0, sizeof(worker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_main_arg = id;
    worker.bgw_notify_pid = MyProcPid;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    initStringInfo(&buf);
    appendStringInfoString(&buf, "pg_task");
    if (buf.len + 1 > BGW_MAXLEN) E("%i > BGW_MAXLEN", buf.len + 1);
    memcpy(worker.bgw_library_name, buf.data, buf.len);
    resetStringInfo(&buf);
    appendStringInfoString(&buf, "task_worker");
    if (buf.len + 1 > BGW_MAXLEN) E("%i > BGW_MAXLEN", buf.len + 1);
    memcpy(worker.bgw_function_name, buf.data, buf.len);
    resetStringInfo(&buf);
    appendStringInfo(&buf, "pg_task %s%s%s %s", work->schema ? work->schema : "", work->schema ? " " : "", work->table, group);
    if (buf.len + 1 > BGW_MAXLEN) E("%i > BGW_MAXLEN", buf.len + 1);
    memcpy(worker.bgw_type, buf.data, buf.len);
    resetStringInfo(&buf);
    appendStringInfo(&buf, "%s %s %s", work->user, work->data, worker.bgw_type);
    if (buf.len + 1 > BGW_MAXLEN) E("%i > BGW_MAXLEN", buf.len + 1);
    memcpy(worker.bgw_name, buf.data, buf.len);
    pfree(buf.data);
    if (user_len + 1 + data_len + 1 + schema_len + 1 + table_len + 1 + oid_len + group_len + 1 + max_len > BGW_EXTRALEN) E("%i > BGW_EXTRALEN", user_len + 1 + data_len + 1 + schema_len + 1 + table_len + 1 + oid_len + group_len + 1 + max_len);
    p = (char *)memcpy(p, work->user, user_len) + user_len + 1;
    p = (char *)memcpy(p, work->data, data_len) + data_len + 1;
    p = (char *)memcpy(p, work->schema, schema_len) + schema_len + 1;
    p = (char *)memcpy(p, work->table, table_len) + table_len + 1;
    p = (char *)memcpy(p, &work->oid, oid_len) + oid_len;
    p = (char *)memcpy(p, group, group_len) + group_len + 1;
    p = (char *)memcpy(p, &max, max_len) + max_len;
    RegisterDynamicBackgroundWorker_my(&worker);
    pfree(group);
}

void tick_timeout(Work *work) {
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    if (!command) {
        StringInfoData buf;
        initStringInfo(&buf);
        appendStringInfo(&buf,
            "WITH s AS (SELECT id FROM %1$s AS t WHERE dt < current_timestamp - concat_ws(' ', (current_setting('pg_task.reset', false)::int4 * current_setting('pg_task.timeout', false)::int4)::text, 'msec')::interval AND state IN ('TAKE'::state, 'WORK'::state) AND pid NOT IN (\n"
            "    SELECT  pid\n"
            "    FROM    pg_stat_activity\n"
            "    WHERE   datname = current_catalog\n"
            "    AND     usename = current_user\n"
            "    AND     application_name = concat_ws(' ', 'pg_task', NULLIF(current_setting('pg_task.schema', true), ''), current_setting('pg_task.table', false), \"group\")\n"
            ") FOR UPDATE SKIP LOCKED) UPDATE %1$s AS u SET state = 'PLAN'::state FROM s WHERE u.id = s.id;\n"
            "WITH s AS (WITH s AS (WITH s AS (WITH s AS (WITH s AS (\n"
            "SELECT      id, \"group\", COALESCE(max, ~(1<<31)) AS max, a.pid\n"
            "FROM        %1$s AS t\n"
            "LEFT JOIN   pg_stat_activity AS a\n"
            "ON          datname = current_catalog\n"
            "AND         usename = current_user\n"
            "AND         application_name = concat_ws(' ', 'pg_task', NULLIF(current_setting('pg_task.schema', true), ''), current_setting('pg_task.table', false), \"group\")\n"
            "WHERE       t.state = 'PLAN'::state\n"
            "AND         dt <= current_timestamp\n"
            ") SELECT id, \"group\", max - count(pid) AS count FROM s GROUP BY id, \"group\", max\n"
            ") SELECT array_agg(id ORDER BY id) AS id, \"group\", count FROM s WHERE count > 0 GROUP BY \"group\", count\n"
            ") SELECT unnest(id[:count]) AS id, \"group\", count FROM s ORDER BY count DESC\n"
            ") SELECT s.* FROM s INNER JOIN %1$s USING (id) FOR UPDATE SKIP LOCKED\n"
            ") UPDATE %1$s AS u SET state = 'TAKE'::state FROM s WHERE u.id = s.id RETURNING u.id, u.group, u.remote, COALESCE(u.max, ~(1<<31)) AS max", work->schema_table);
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, 0, NULL);
    SPI_execute_plan_my(plan, NULL, NULL, SPI_OK_UPDATE_RETURNING, true);
    for (uint64 row = 0; row < SPI_processed; row++) {
        int64 id = DatumGetInt64(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "id", false));
        int max = DatumGetInt32(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "max", false));
        MemoryContext oldMemoryContext = MemoryContextSwitchTo(TopMemoryContext);
        char *group = TextDatumGetCStringMy(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "group", false));
        char *remote = TextDatumGetCStringMy(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "remote", true));
        MemoryContextSwitchTo(oldMemoryContext);
        D1("row = %lu, id = %li, group = %s, remote = %s, max = %i", row, id, group, remote ? remote : null, max);
        if (remote) tick_remote(work, id, group, remote, max); else tick_task(work, id, group, max);
    }
    SPI_finish_my();
}

static bool tick_check(void) {
    bool exit = false;
    static SPI_plan *plan = NULL;
    static const char *command =
        "WITH s AS ("
        "SELECT      COALESCE(COALESCE(usename, \"user\"), data)::text AS user,\n"
        "            COALESCE(datname, data)::text AS data,\n"
        "            schema,\n"
        "            COALESCE(\"table\", current_setting('pg_task.default_table', false)) AS table,\n"
        "            COALESCE(reset, current_setting('pg_task.default_reset', false)::int4) AS reset,\n"
        "            COALESCE(timeout, current_setting('pg_task.default_timeout', false)::int4) AS timeout\n"
        "FROM        json_populate_recordset(NULL::record, current_setting('pg_task.json', false)::json) AS s (\"user\" text, data text, schema text, \"table\" text, reset int4, timeout int4)\n"
        "LEFT JOIN   pg_database AS d ON (data IS NULL OR datname = data) AND NOT datistemplate AND datallowconn\n"
        "LEFT JOIN   pg_user AS u ON usename = COALESCE(COALESCE(\"user\", (SELECT usename FROM pg_user WHERE usesysid = datdba)), data)\n"
        ") SELECT DISTINCT * FROM s WHERE \"user\" = current_user AND data = current_catalog AND schema IS NOT DISTINCT FROM NULLIF(current_setting('pg_task.schema', true), '') AND \"table\" = current_setting('pg_task.table', false) AND reset = current_setting('pg_task.reset', false)::int4 AND timeout = current_setting('pg_task.timeout', false)::int4";
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, 0, NULL);
    SPI_execute_plan_my(plan, NULL, NULL, SPI_OK_SELECT, true);
    if (!SPI_processed) exit = true;
    SPI_finish_my();
    return exit;
}

static void tick_init_conf(Work *work) {
    char *p = MyBgworkerEntry->bgw_extra;
    work->user = p;
    p += strlen(work->user) + 1;
    work->data = p;
    p += strlen(work->data) + 1;
    work->schema = p;
    p += strlen(work->schema) + 1;
    work->table = p;
    p += strlen(work->table) + 1;
    work->reset = *(typeof(work->reset) *)p;
    p += sizeof(work->reset);
    work->timeout = *(typeof(work->timeout) *)p;
    if (work->table == work->schema + 1) work->schema = NULL;
    if (!MyProcPort && !(MyProcPort = (Port *) calloc(1, sizeof(Port)))) E("!calloc");
    if (!MyProcPort->remote_host) MyProcPort->remote_host = "[local]";
    if (!MyProcPort->user_name) MyProcPort->user_name = work->user;
    if (!MyProcPort->database_name) MyProcPort->database_name = work->data;
    null = GetConfigOption("pg_task.null", false, true);
    SetConfigOptionMy("application_name", MyBgworkerEntry->bgw_type);
    D1("user = %s, data = %s, schema = %s, table = %s, reset = %i, timeout = %i", work->user, work->data, work->schema ? work->schema : null, work->table, work->reset, work->timeout);
    pqsignal(SIGHUP, init_sighup);
    pqsignal(SIGTERM, init_sigterm);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection(work->data, work->user, 0);
    pgstat_report_appname(MyBgworkerEntry->bgw_type);
    process_session_preload_libraries();
}

bool tick_init(Work *work) {
    bool exit = false;
    const char *schema_quote = work->schema ? quote_identifier(work->schema) : NULL;
    const char *table_quote = quote_identifier(work->table);
    StringInfoData buf;
    MemoryContext oldMemoryContext = MemoryContextSwitchTo(TopMemoryContext);
    initStringInfo(&buf);
    MemoryContextSwitchTo(oldMemoryContext);
    if (work->schema) appendStringInfo(&buf, "%s.", schema_quote);
    appendStringInfoString(&buf, table_quote);
    if (work->schema_table) pfree(work->schema_table);
    work->schema_table = buf.data;
    if (work->schema && schema_quote && work->schema != schema_quote) pfree((void *)schema_quote);
    if (work->table != table_quote) pfree((void *)table_quote);
    D1("user = %s, data = %s, schema = %s, table = %s, reset = %i, timeout = %i, schema_table = %s", work->user, work->data, work->schema ? work->schema : null, work->table, work->reset, work->timeout, work->schema_table);
    if (work->schema) tick_schema(work);
    tick_type(work);
    if (tick_table(work)) return true;
    tick_index(work, "dt");
    tick_index(work, "state");
    SetConfigOptionMy("pg_task.data", work->data);
    SetConfigOptionMy("pg_task.user", work->user);
    initStringInfo(&buf);
    appendStringInfo(&buf, "%i", work->reset);
    SetConfigOptionMy("pg_task.reset", buf.data);
    resetStringInfo(&buf);
    appendStringInfo(&buf, "%i", work->timeout);
    SetConfigOptionMy("pg_task.timeout", buf.data);
    pfree(buf.data);
    queue_init(&work->queue);
    return exit;
}

static bool tick_reload(void) {
    sighup = false;
    ProcessConfigFile(PGC_SIGHUP);
    return tick_check();
}

static bool tick_latch(void) {
    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();
    if (sighup) return tick_reload();
    return false;
}

static void tick_command(Task *task, PGresult *result) {
    if (task->skip) { task->skip--; return; }
    if (!task->response.data) initStringInfo(&task->response);
    appendStringInfo(&task->response, "%s%s", task->response.len ? "\n" : "", PQcmdStatus(result));
}

static void tick_success(Task *task, PGresult *result) {
    if (task->length == 1 && !PQntuples(result)) return;
    if (!task->response.data) initStringInfo(&task->response);
    if (task->header && (task->length > 1 || PQnfields(result) > 1)) {
        if (task->response.len) appendStringInfoString(&task->response, "\n");
        for (int col = 0; col < PQnfields(result); col++) {
            const char *value = PQfname(result, col);
            if (col > 0) appendStringInfoChar(&task->response, task->delimiter);
            if (task->quote) appendStringInfoChar(&task->response, task->quote);
            if (task->escape) init_escape(&task->response, value, strlen(value), task->escape);
            else appendStringInfoString(&task->response, value);
            if (task->append && !strstr(value, "::")) {
                Oid oid = PQftype(result, col);
                const char *type = PQftypeMy(oid);
                if (task->escape) init_escape(&task->response, "::", sizeof("::") - 1, task->escape);
                else appendStringInfoString(&task->response, "::");
                if (type) {
                    if (task->escape) init_escape(&task->response, type, strlen(type), task->escape);
                    else appendStringInfoString(&task->response, type);
                } else appendStringInfo(&task->response, "%i", oid);
            }
            if (task->quote) appendStringInfoChar(&task->response, task->quote);
        }
    }
    for (int row = 0; row < PQntuples(result); row++) {
        if (task->response.len) appendStringInfoString(&task->response, "\n");
        for (int col = 0; col < PQnfields(result); col++) {
            const char *value = PQgetvalue(result, row, col);
            int len = PQgetlength(result, row, col);
            if (col > 0) appendStringInfoChar(&task->response, task->delimiter);
            if (PQgetisnull(result, row, col)) appendStringInfoString(&task->response, task->null); else {
                if (!init_oid_is_string(PQftype(result, col)) && task->string) {
                    if (len) appendStringInfoString(&task->response, value);
                } else {
                    if (task->quote) appendStringInfoChar(&task->response, task->quote);
                    if (len) {
                        if (task->escape) init_escape(&task->response, value, len, task->escape);
                        else appendStringInfoString(&task->response, value);
                    }
                    if (task->quote) appendStringInfoChar(&task->response, task->quote);
                }
            }
        }
    }
}

static void tick_fail(Task *task, PGresult *result) {
    char *value;
    if (!task->response.data) initStringInfo(&task->response);
    if ((value = PQresultErrorField(result, PG_DIAG_SEVERITY))) appendStringInfo(&task->response, "%sseverity%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SEVERITY_NONLOCALIZED))) appendStringInfo(&task->response, "%sseverity_nonlocalized%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SQLSTATE))) appendStringInfo(&task->response, "%ssqlstate%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY))) appendStringInfo(&task->response, "%smessage_primary%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_MESSAGE_DETAIL))) appendStringInfo(&task->response, "%smessage_detail%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_MESSAGE_HINT))) appendStringInfo(&task->response, "%smessage_hint%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_STATEMENT_POSITION))) appendStringInfo(&task->response, "%sstatement_position%s%c%s", task->response.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_INTERNAL_POSITION))) appendStringInfo(&task->response, "%sinternal_position%s%c%s", task->response.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_INTERNAL_QUERY))) appendStringInfo(&task->response, "%sinternal_query%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_CONTEXT))) appendStringInfo(&task->response, "%scontext%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SCHEMA_NAME))) appendStringInfo(&task->response, "%sschema_name%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_TABLE_NAME))) appendStringInfo(&task->response, "%stable_name%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_COLUMN_NAME))) appendStringInfo(&task->response, "%scolumn_name%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_DATATYPE_NAME))) appendStringInfo(&task->response, "%sdatatype_name%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_CONSTRAINT_NAME))) appendStringInfo(&task->response, "%sconstraint_name%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SOURCE_FILE))) appendStringInfo(&task->response, "%ssource_file%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SOURCE_LINE))) appendStringInfo(&task->response, "%ssource_line%s%c%s", task->response.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SOURCE_FUNCTION))) appendStringInfo(&task->response, "%ssource_function%s%c%s", task->response.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    appendStringInfo(&task->response, "%sROLLBACK", task->response.len ? "\n" : "");
    task->skip++;
    task->fail = true;
}

static void tick_query(Task *task) {
    StringInfoData buf;
    List *list;
    if (task_work(task)) { tick_finish(task); return; }
    D1("id = %li, timeout = %i, request = %s, count = %i", task->id, task->timeout, task->request, task->count);
    PG_TRY();
        list = pg_parse_query(task->request);
        task->length = list_length(list);
        list_free_deep(list);
    PG_CATCH();
        FlushErrorState();
    PG_END_TRY();
    initStringInfo(&buf);
    task->skip = 0;
    appendStringInfo(&buf, "SET \"pg_task.id\" = %li;\n", task->id);
    task->skip++;
    if (task->timeout) {
        appendStringInfo(&buf, "SET \"statement_timeout\" = %i;\n", task->timeout);
        task->skip++;
    }
    if (task->append) {
        appendStringInfoString(&buf, "SET \"config.append_type_to_column_name\" = true;\n");
        task->skip++;
    }
    appendStringInfoString(&buf, task->request);
    pfree(task->request);
    task->request = buf.data;
    if (!PQsendQuery(task->conn, task->request)) tick_error(task, "!PQsendQuery"); else {
        pfree(task->request);
        task->request = NULL;
        task->events = WL_SOCKET_WRITEABLE;
    }
}

static void tick_repeat(Task *task) {
    if (PQtransactionStatus(task->conn) != PQTRANS_IDLE) { if (!PQsendQuery(task->conn, "COMMIT")) tick_error(task, "!PQsendQuery"); else task->events = WL_SOCKET_WRITEABLE; return; }
    if (task_done(task)) { tick_finish(task); return; }
    D1("repeat = %s, delete = %s, live = %s", task->repeat ? "true" : "false", task->delete ? "true" : "false", task->live ? "true" : "false");
    if (task->repeat) task_repeat(task);
    if (task->delete && !task->response.data) task_delete(task);
    if (task->response.data) pfree(task->response.data);
    task->response.data = NULL;
    if (!task->live || task_live(task)) tick_finish(task); else tick_query(task);
}

static void tick_result(Task *task) {
    if (!PQconsumeInput(task->conn)) tick_error(task, "!PQconsumeInput"); else
    if (PQisBusy(task->conn)) task->events = WL_SOCKET_READABLE; else {
        for (PGresult *result; (result = PQgetResult(task->conn)); PQclear(result)) switch (PQresultStatus(result)) {
            case PGRES_COMMAND_OK: tick_command(task, result); break;
            case PGRES_FATAL_ERROR: tick_fail(task, result); break;
            case PGRES_TUPLES_OK: tick_success(task, result); break;
            default: D1(PQresStatus(PQresultStatus(result))); break;
        }
        tick_repeat(task);
    }
}

static void tick_connect(Task *task) {
    switch (PQstatus(task->conn)) {
        case CONNECTION_AUTH_OK: D1("PQstatus == CONNECTION_AUTH_OK"); break;
        case CONNECTION_AWAITING_RESPONSE: D1("PQstatus == CONNECTION_AWAITING_RESPONSE"); break;
        case CONNECTION_BAD: D1("PQstatus == CONNECTION_BAD"); tick_error(task, "PQstatus == CONNECTION_BAD"); return;
#if (PG_VERSION_NUM >= 130000)
        case CONNECTION_CHECK_TARGET: D1("PQstatus == CONNECTION_CHECK_TARGET"); break;
#endif
        case CONNECTION_CHECK_WRITABLE: D1("PQstatus == CONNECTION_CHECK_WRITABLE"); break;
        case CONNECTION_CONSUME: D1("PQstatus == CONNECTION_CONSUME"); break;
        case CONNECTION_GSS_STARTUP: D1("PQstatus == CONNECTION_GSS_STARTUP"); break;
        case CONNECTION_MADE: D1("PQstatus == CONNECTION_MADE"); break;
        case CONNECTION_NEEDED: D1("PQstatus == CONNECTION_NEEDED"); break;
        case CONNECTION_OK: D1("PQstatus == CONNECTION_OK"); task->connected = true; break;
        case CONNECTION_SETENV: D1("PQstatus == CONNECTION_SETENV"); break;
        case CONNECTION_SSL_STARTUP: D1("PQstatus == CONNECTION_SSL_STARTUP"); break;
        case CONNECTION_STARTED: D1("PQstatus == CONNECTION_STARTED"); break;
    }
    switch (PQconnectPoll(task->conn)) {
        case PGRES_POLLING_ACTIVE: D1("PQconnectPoll == PGRES_POLLING_ACTIVE"); break;
        case PGRES_POLLING_FAILED: D1("PQconnectPoll == PGRES_POLLING_FAILED"); tick_error(task, "PQconnectPoll == PGRES_POLLING_FAILED"); return;
        case PGRES_POLLING_OK: D1("PQconnectPoll == PGRES_POLLING_OK"); task->connected = true; break;
        case PGRES_POLLING_READING: D1("PQconnectPoll == PGRES_POLLING_READING"); task->events = WL_SOCKET_READABLE; break;
        case PGRES_POLLING_WRITING: D1("PQconnectPoll == PGRES_POLLING_WRITING"); task->events = WL_SOCKET_WRITEABLE; break;
    }
    if ((task->fd = PQsocket(task->conn)) < 0) tick_error(task, "PQsocket < 0");
    if (task->connected) {
        if (!(task->pid = PQbackendPID(task->conn))) tick_error(task, "!PQbackendPID"); else tick_query(task);
    }
}

void tick_socket(Task *task) {
    D1("connected = %s", task->connected ? "true" : "false");
    if (task->connected) tick_result(task); else tick_connect(task);
}

void tick_worker(Datum main_arg); void tick_worker(Datum main_arg) {
    TimestampTz stop = GetCurrentTimestamp(), start = stop;
    Work work;
    if (StandbyMode) { W("StandbyMode"); return; }
    MemSet(&work, 0, sizeof(work));
    tick_init_conf(&work);
    sigterm = sigterm || tick_init(&work);
    while (!sigterm && !StandbyMode) {
        int nevents = queue_size(&work.queue) + 2;
        WaitEvent *events = palloc0(nevents * sizeof(*events));
        WaitEventSet *set = CreateWaitEventSet(TopMemoryContext, nevents);
        AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
        AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
        queue_each(&work.queue, queue) {
            Task *task = queue_data(queue, Task, queue);
            if (task->events & WL_SOCKET_WRITEABLE) switch (PQflush(task->conn)) {
                case 0: /*D1("PQflush = 0");*/ break;
                case 1: D1("PQflush = 1"); break;
                default: D1("PQflush = default"); break;
            }
            AddWaitEventToSet(set, task->events & WL_SOCKET_MASK, task->fd, NULL, task);
        }
        nevents = WaitEventSetWait(set, work.timeout, events, nevents, PG_WAIT_EXTENSION);
        for (int i = 0; i < nevents; i++) {
            WaitEvent *event = &events[i];
            if (event->events & WL_LATCH_SET) D1("WL_LATCH_SET");
            if (event->events & WL_SOCKET_READABLE) D1("WL_SOCKET_READABLE");
            if (event->events & WL_SOCKET_WRITEABLE) D1("WL_SOCKET_WRITEABLE");
            if (event->events & WL_POSTMASTER_DEATH) D1("WL_POSTMASTER_DEATH");
            if (event->events & WL_EXIT_ON_PM_DEATH) D1("WL_EXIT_ON_PM_DEATH");
            if (event->events & WL_LATCH_SET) sigterm = sigterm || tick_latch();
            if (event->events & WL_SOCKET_MASK) tick_socket(event->user_data);
        }
        stop = GetCurrentTimestamp();
        if (work.timeout > 0 && (TimestampDifferenceExceeds(start, stop, work.timeout) || !nevents)) {
            tick_timeout(&work);
            start = stop;
        }
        FreeWaitEventSet(set);
        pfree(events);
    }
}
