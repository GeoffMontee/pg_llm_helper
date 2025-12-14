#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include "tcop/tcopprot.h"
#include "access/xact.h"
#include "lib/stringinfo.h"
#include "datatype/timestamp.h"
#include <time.h>

PG_MODULE_MAGIC;

/* Maximum number of errors to store in circular buffer */
#define MAX_ERRORS 100
#define MAX_QUERY_LEN 8192
#define MAX_ERROR_MSG_LEN 1024

/* Structure to hold error information */
typedef struct ErrorEntry
{
    int32 backend_pid;
    char query_text[MAX_QUERY_LEN];
    char error_message[MAX_ERROR_MSG_LEN];
    char sql_state[6];
    int error_level;
    TimestampTz timestamp;
} ErrorEntry;

/* Shared memory structure */
typedef struct ErrorBuffer
{
    LWLock *lock;
    int current_index;
    int total_errors;
    ErrorEntry errors[MAX_ERRORS];
} ErrorBuffer;

/* Global variables */
static ErrorBuffer *error_buffer = NULL;
static emit_log_hook_type prev_emit_log_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

/* Function declarations */
void _PG_init(void);
void _PG_fini(void);

static void llm_helper_emit_log(ErrorData *edata);
static void llm_helper_shmem_startup(void);
static void llm_helper_shmem_request(void);
static Size llm_helper_shmem_size(void);

PG_FUNCTION_INFO_V1(get_last_error);
PG_FUNCTION_INFO_V1(get_error_history);
PG_FUNCTION_INFO_V1(clear_error_history);

/* Context for get_error_history set-returning function */
typedef struct
{
    int next_index;
    int limit;
    ErrorEntry *entries;
} ErrorHistoryContext;

/*
 * Module load callback
 */
void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
        return;

    /* Install hooks */
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = llm_helper_shmem_request;

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = llm_helper_shmem_startup;

    prev_emit_log_hook = emit_log_hook;
    emit_log_hook = llm_helper_emit_log;

    elog(LOG, "pg_llm_helper loaded");
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
    /* Restore hooks */
    shmem_request_hook = prev_shmem_request_hook;
    emit_log_hook = prev_emit_log_hook;
    shmem_startup_hook = prev_shmem_startup_hook;
}

/*
 * Shared memory request hook - called during postmaster startup
 */
static void
llm_helper_shmem_request(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();

    RequestAddinShmemSpace(llm_helper_shmem_size());
    RequestNamedLWLockTranche("pg_llm_helper", 1);
}

/*
 * Calculate shared memory size needed
 */
static Size
llm_helper_shmem_size(void)
{
    Size size;

    size = MAXALIGN(sizeof(ErrorBuffer));
    return size;
}

/*
 * Shared memory initialization
 */
static void
llm_helper_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    /* Reset in case this is a restart */
    error_buffer = NULL;

    /* Create or attach to shared memory */
    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    error_buffer = ShmemInitStruct("pg_llm_helper",
                                   llm_helper_shmem_size(),
                                   &found);

    if (!found)
    {
        /* Initialize shared memory */
        error_buffer->lock = &(GetNamedLWLockTranche("pg_llm_helper"))->lock;
        error_buffer->current_index = 0;
        error_buffer->total_errors = 0;
        memset(error_buffer->errors, 0, sizeof(error_buffer->errors));
    }

    LWLockRelease(AddinShmemInitLock);
}

/*
 * Hook to capture log messages and errors
 */
static void
llm_helper_emit_log(ErrorData *edata)
{
    /* Only capture errors and warnings */
    if (edata->elevel >= ERROR && error_buffer != NULL)
    {
        ErrorEntry *entry;
        const char *query;

        LWLockAcquire(error_buffer->lock, LW_EXCLUSIVE);

        /* Get next slot in circular buffer */
        entry = &error_buffer->errors[error_buffer->current_index];
        error_buffer->current_index = (error_buffer->current_index + 1) % MAX_ERRORS;
        error_buffer->total_errors++;

        /* Store error information */
        entry->backend_pid = MyProcPid;
        entry->error_level = edata->elevel;
        entry->timestamp = GetCurrentTimestamp();

        /* Copy SQL state */
        if (edata->sqlerrcode)
            snprintf(entry->sql_state, sizeof(entry->sql_state), 
                    "%s", unpack_sql_state(edata->sqlerrcode));
        else
            entry->sql_state[0] = '\0';

        /* Copy error message */
        if (edata->message)
            strlcpy(entry->error_message, edata->message, MAX_ERROR_MSG_LEN);
        else
            entry->error_message[0] = '\0';

        /* Copy query text */
        query = debug_query_string ? debug_query_string : "";
        strlcpy(entry->query_text, query, MAX_QUERY_LEN);

        LWLockRelease(error_buffer->lock);
    }

    /* Call previous hook if exists */
    if (prev_emit_log_hook)
        prev_emit_log_hook(edata);
}

/*
 * SQL function: get_last_error()
 * Returns the most recent error for the current backend
 */
Datum
get_last_error(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[6];
    bool nulls[6];
    HeapTuple tuple;
    int i;
    int my_pid = MyProcPid;
    ErrorEntry *latest = NULL;
    TimestampTz latest_time = 0;

    if (error_buffer == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_llm_helper shared memory not initialized")));

    /* Build tuple descriptor */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context that cannot accept type record")));

    LWLockAcquire(error_buffer->lock, LW_SHARED);

    /* Find most recent error for this backend */
    for (i = 0; i < MAX_ERRORS; i++)
    {
        ErrorEntry *entry = &error_buffer->errors[i];
        
        if (entry->backend_pid == my_pid && 
            entry->timestamp > 0 &&
            entry->timestamp > latest_time)
        {
            latest = entry;
            latest_time = entry->timestamp;
        }
    }

    if (latest == NULL)
    {
        LWLockRelease(error_buffer->lock);
        PG_RETURN_NULL();
    }

    /* Build result tuple */
    memset(nulls, 0, sizeof(nulls));
    values[0] = Int32GetDatum(latest->backend_pid);
    values[1] = CStringGetTextDatum(latest->query_text);
    values[2] = CStringGetTextDatum(latest->error_message);
    values[3] = CStringGetTextDatum(latest->sql_state);
    values[4] = Int32GetDatum(latest->error_level);
    values[5] = TimestampTzGetDatum(latest->timestamp);

    LWLockRelease(error_buffer->lock);

    tuple = heap_form_tuple(tupdesc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * SQL function: get_error_history(max_results int)
 * Returns recent errors across all backends
 */
Datum
get_error_history(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    int32 limit;
    MemoryContext oldcontext;
    ErrorHistoryContext *ctx;
    TupleDesc tupdesc;

    if (SRF_IS_FIRSTCALL())
    {
        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        limit = PG_GETARG_INT32(0);
        if (limit <= 0 || limit > MAX_ERRORS)
            limit = MAX_ERRORS;

        ctx = palloc(sizeof(ErrorHistoryContext));
        ctx->limit = limit;
        ctx->next_index = 0;
        ctx->entries = palloc(sizeof(ErrorEntry) * MAX_ERRORS);

        /* Copy entries from shared memory */
        LWLockAcquire(error_buffer->lock, LW_SHARED);
        memcpy(ctx->entries, error_buffer->errors, sizeof(ErrorEntry) * MAX_ERRORS);
        LWLockRelease(error_buffer->lock);

        funcctx->user_fctx = ctx;

        /* Build tuple descriptor */
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("function returning record called in context that cannot accept type record")));
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    ctx = (ErrorHistoryContext *) funcctx->user_fctx;

    /* Find next valid entry */
    while (ctx->next_index < MAX_ERRORS && funcctx->call_cntr < ctx->limit)
    {
        ErrorEntry *entry = &ctx->entries[ctx->next_index++];
        
        if (entry->timestamp > 0)
        {
            Datum values[6];
            bool nulls[6];
            HeapTuple tuple;

            memset(nulls, 0, sizeof(nulls));
            values[0] = Int32GetDatum(entry->backend_pid);
            values[1] = CStringGetTextDatum(entry->query_text);
            values[2] = CStringGetTextDatum(entry->error_message);
            values[3] = CStringGetTextDatum(entry->sql_state);
            values[4] = Int32GetDatum(entry->error_level);
            values[5] = TimestampTzGetDatum(entry->timestamp);

            tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
            SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
        }
    }

    SRF_RETURN_DONE(funcctx);
}

/*
 * SQL function: clear_error_history()
 * Clears all stored errors
 */
Datum
clear_error_history(PG_FUNCTION_ARGS)
{
    if (error_buffer == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_llm_helper shared memory not initialized")));

    LWLockAcquire(error_buffer->lock, LW_EXCLUSIVE);
    
    error_buffer->current_index = 0;
    error_buffer->total_errors = 0;
    memset(error_buffer->errors, 0, sizeof(error_buffer->errors));
    
    LWLockRelease(error_buffer->lock);

    PG_RETURN_VOID();
}
