/*
 * pgsentinel.c
 *   Track active session history.
 *
 * Copyright (c) 2018, PgSentinel
 *
 * IDENTIFICATION:
 * https://github.com/pgsentinel/pgsentinel
 *
 * LICENSE: GNU Affero General Public License v3.0
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/xact.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "executor/spi.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "miscadmin.h"
#include "storage/spin.h"
#include "utils/date.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/nabstime.h"
#include "funcapi.h"
#include "optimizer/planner.h"
#include "storage/shm_toc.h"
#include "access/twophase.h"
#include "parser/analyze.h"
#include "parser/scansup.h"
#include "access/hash.h"

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(pg_active_session_history);

#define PG_ACTIVE_SESSION_HISTORY_COLS        23 

/* Entry point of library loading */
void _PG_init(void);
void pgsentinel_main(Datum);

/* Signal handling */
static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sighup = false;

/* Saved hook values in case of unload */
static shmem_startup_hook_type ash_prev_shmem_startup_hook = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;


/* Our hooks */
static void ash_shmem_startup(void);
static void ash_shmem_shutdown(int code, Datum arg);
static void ash_post_parse_analyze(ParseState *pstate, Query *query);

/* GUC variables */
static int ash_sampling_period = 1;
static int ash_max_entries = 1000;
char *pgsentinelDbName = "postgres";

/* to create queryid in case of utility statements*/
#if PG_VERSION_NUM >= 110000
static uint64 ash_hash64_string(const char *str, int len);
#else
static uint32 ash_hash32_string(const char *str, int len);
#endif

/* Worker name */
static char *worker_name = "pgsentinel";

/* pg_stat_activity query */
static const char * const pg_stat_activity_query=
"select datid, datname, pid, usesysid, usename, application_name, text(client_addr), client_hostname, client_port, backend_start, xact_start, query_start, state_change, case when wait_event_type is null then 'CPU' else wait_event_type end as wait_event_type,case when wait_event is null then 'CPU' else wait_event end as wait_event, state, backend_xid, backend_xmin, query, backend_type from pg_stat_activity where state='active' and pid != pg_backend_pid()";

static void pg_active_session_history_internal(FunctionCallInfo fcinfo);

/* ash entry */
typedef struct ashEntry
{
        int pid;
        uint64 queryid;
        TimestampTz ash_time;
	Oid *datid;
	Oid *usesysid;
	int *client_port;
	char *usename;	
	char *datname;	
	char *application_name;	
	char *wait_event_type;	
	char *wait_event;	
	char *state;	
	char *client_hostname;	
	char *top_level_query;	
	char *query;	
	char *backend_type;	
	char *client_addr;	
	TransactionId *backend_xmin;
	TransactionId *backend_xid;
        TimestampTz backend_start;
        TimestampTz xact_start;
        TimestampTz query_start;
        TimestampTz state_change;
} ashEntry;

/* Proc entry */
typedef struct procEntry
{
        uint64 queryid;
	char *query;
	int qlen;
} procEntry;


/* For shared memory */
static char *AshEntryUsenameBuffer = NULL;
static char *AshEntryDatnameBuffer = NULL;
static char *AshEntryAppnameBuffer = NULL;
static char *AshEntryWaitEventTypeBuffer = NULL;
static char *AshEntryWaitEventBuffer = NULL;
static char *AshEntryClientHostnameBuffer = NULL;
static char *AshEntryTopLevelQueryBuffer = NULL;
static char *AshEntryQueryBuffer = NULL;
static char *AshEntryBackendTypeBuffer = NULL;
static char *AshEntryStateBuffer = NULL;
static char *AshEntryClientaddrBuffer = NULL;
static ashEntry *AshEntryArray = NULL;
static procEntry *ProcEntryArray = NULL;
static char *ProcQueryBuffer = NULL;


/* Estimate amount of shared memory needed */
static Size ash_entry_memsize(void);
static Size proc_entry_memsize(void);

/* Search proc from backend */
static procEntry search_procentry(int backendPid);

/* store ash entry */
static void ash_entry_store(TimestampTz ash_time, int inserted,const int pid,const char *usename, const int *client_port,Oid *datid, const char *datname, const char *application_name, const char *client_addr, TransactionId *backend_xmin, TimestampTz backend_start, TimestampTz xact_start, TimestampTz query_start, TimestampTz state_change, const char *wait_event_type, const char *wait_event, const char *state, const char *client_hostname, const char *query, const char *backend_type, Oid *usesysid, TransactionId *backend_xid);

/* to rotate the ash entries */
static int inserted=0;

/* prepare store ash */
static void ash_prepare_store(TimestampTz ash_time,const int pid, const char* usename,const int *client_port, Oid *datid, const char *datname, const char *application_name, const char *client_addr, TransactionId *backend_xmin, TimestampTz backend_start, TimestampTz xact_start, TimestampTz query_start, TimestampTz state_change, const char *wait_event_type, const char *wait_event, const char *state, const char *client_hostname, const char *query, const char *backend_type, Oid *usesysid, TransactionId *backend_xid);

/* get max procs */
static int get_max_procs_count(void);

/* The procEntry */
static procEntry
search_procentry(int pid)
{
        int i;

        for (i = 0; i < ProcGlobal->allProcCount; i++)
        {
                PGPROC  *proc = &ProcGlobal->allProcs[i];
                if (proc != NULL && proc->pid != 0 && proc->pid == pid)
                {
                        return ProcEntryArray[i];
                }
        }

        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                        errmsg("backend with pid=%d not found", pid)));
}

/* to create queryid in case of utility statements*/
#if PG_VERSION_NUM >= 110000
static uint64
ash_hash64_string(const char *str, int len)
{
        return DatumGetUInt64(hash_any_extended((const unsigned char *) str,
                                                                                        len, 0));
}
#else
static uint32
ash_hash32_string(const char *str, int len)
{
        return hash_any((const unsigned char *) str, len);
}
#endif

/*
 * Calculate max processes count.
 */
static int
get_max_procs_count(void)
{
        int count = 0;

        /* MyProcs, including autovacuum workers and launcher */
        count += MaxBackends;
        /* AuxiliaryProcs */
        count += NUM_AUXILIARY_PROCS;
        /* Prepared xacts */
        count += max_prepared_xacts;

        return count;
}

/* save queryid and query text */
static void
ash_post_parse_analyze(ParseState *pstate, Query *query)
{

        if (prev_post_parse_analyze_hook)
                prev_post_parse_analyze_hook(pstate, query);

        if (MyProc)
        {
        int i = MyProc - ProcGlobal->allProcs;
        const char *querytext = pstate->p_sourcetext;
        int query_location = query->stmt_location;
        int query_len = query->stmt_len;
        int minlen;

        if (query_location >= 0)
        {
                Assert(query_location <= strlen(querytext));
                querytext += query_location;
                /* Length of 0 (or -1) means "rest of string" */
                if (query_len <= 0)
                        query_len = strlen(querytext);
                else
                        Assert(query_len <= strlen(querytext));
        }
        else
        {
                /* If query location is unknown, distrust query_len as well */
                query_location = 0;
                query_len = strlen(querytext);
        }

         /*
         * Discard leading and trailing whitespace, too.  Use scanner_isspace()
         * not libc's isspace(), because we want to match the lexer's behavior.
         */
        while (query_len > 0 && scanner_isspace(querytext[0]))
                querytext++, query_location++, query_len--;
        while (query_len > 0 && scanner_isspace(querytext[query_len - 1]))
                query_len--;

        minlen = Min(query_len,pgstat_track_activity_query_size-1);
        memcpy(ProcEntryArray[i].query,querytext,minlen);
        ProcEntryArray[i].qlen=minlen;
        /*
         * For utility statements, we just hash the query string to get an ID.
         */
#if PG_VERSION_NUM >= 110000
	if (query->queryId == UINT64CONST(0)) {
                ProcEntryArray[i].queryid = ash_hash64_string(querytext, query_len);
#else
        if (query->queryId == 0) {
                ProcEntryArray[i].queryid = ash_hash32_string(querytext, query_len);
#endif
        } else {
        ProcEntryArray[i].queryid = query->queryId;
        }
        }
}

/* Estimate amount of shared memory needed for ash entry*/
static Size
ash_entry_memsize(void)
{
        Size            size;

        /* AshEntryArray */
        size = mul_size(sizeof(ashEntry), ash_max_entries);
        /* AshEntryUsenameBuffer */
        size = add_size(size, mul_size(NAMEDATALEN, ash_max_entries));
	/* AshEntryDatnameBuffer */
        size = add_size(size, mul_size(NAMEDATALEN, ash_max_entries));
	/* AshEntryAppnameBuffer */
        size = add_size(size, mul_size(NAMEDATALEN, ash_max_entries));
	/* AshEntryClientaddrBuffer */
        size = add_size(size, mul_size(NAMEDATALEN, ash_max_entries));
	/* AshEntryWaitEventTypeBuffer */
        size = add_size(size, mul_size(NAMEDATALEN, ash_max_entries));
	/* AshEntryWaitEventBuffer */
        size = add_size(size, mul_size(NAMEDATALEN, ash_max_entries));
	/* AshEntryStateBuffer */
        size = add_size(size, mul_size(NAMEDATALEN, ash_max_entries));
	/* AshEntryClientHostnameBuffer */
        size = add_size(size, mul_size(NAMEDATALEN, ash_max_entries));
	/* AshEntryQueryBuffer */
        size = add_size(size, mul_size(pgstat_track_activity_query_size, ash_max_entries));
	/* AshEntryTopLevelQueryBuffer */
        size = add_size(size, mul_size(pgstat_track_activity_query_size, ash_max_entries));
	/* AshEntryBackendTypeBuffer */
        size = add_size(size, mul_size(NAMEDATALEN, ash_max_entries));
	return size;
}

/* Estimate amount of shared memory needed for proc entry*/
static Size
proc_entry_memsize(void)
{
        Size            size;

        /* ProcEntryArray */
        size = mul_size(sizeof(procEntry), get_max_procs_count());
        /* ProEntryQueryBuffer */
        size = add_size(size, mul_size(pgstat_track_activity_query_size, get_max_procs_count()));
        return size;
}


static void
ash_shmem_startup(void)
{

	Size size;
        bool   found;
	char   *buffer;
	int    i;

	if (ash_prev_shmem_startup_hook) 
            	ash_prev_shmem_startup_hook();

	size = mul_size(sizeof(ashEntry), ash_max_entries);	
	AshEntryArray = (ashEntry *) ShmemInitStruct("Ash Entry Array", size, &found);	

	if (!found)
        {
         MemSet(AshEntryArray, 0, size);
        }

	size = mul_size(sizeof(procEntry), get_max_procs_count());
        ProcEntryArray = (procEntry *) ShmemInitStruct("Proc Entry Array", size, &found);

        if (!found)
        {
         MemSet(ProcEntryArray, 0, size);
        }

        size = mul_size(pgstat_track_activity_query_size, get_max_procs_count());
        ProcQueryBuffer = (char *) ShmemInitStruct("Proc Query Buffer", size, &found);

        if (!found)
        {
                MemSet(ProcQueryBuffer, 0, size);

                /* Initialize pointers. */
                buffer = ProcQueryBuffer;
                for (i = 0; i < get_max_procs_count(); i++)
                                        {
                        ProcEntryArray[i].query= buffer;
                        buffer += pgstat_track_activity_query_size;
                }
        }

        size = mul_size(NAMEDATALEN, ash_max_entries);
        AshEntryUsenameBuffer = (char *)
                ShmemInitStruct("Ash Entry useName Buffer", size, &found);

	if (!found)
        {
                MemSet(AshEntryUsenameBuffer, 0, size);

                /* Initialize usename pointers. */
                buffer = AshEntryUsenameBuffer;
                for (i = 0; i < ash_max_entries; i++)
			                {
                        AshEntryArray[i].usename = buffer;
                        buffer += NAMEDATALEN;
                }
        }


        size = mul_size(NAMEDATALEN, ash_max_entries);
        AshEntryDatnameBuffer = (char *)
                ShmemInitStruct("Ash Entry DatName Buffer", size, &found);

	if (!found)
        {
                MemSet(AshEntryDatnameBuffer, 0, size);

                /* Initialize Datname pointers. */
                buffer = AshEntryDatnameBuffer;
                for (i = 0; i < ash_max_entries; i++)
			                {
                        AshEntryArray[i].datname = buffer;
                        buffer += NAMEDATALEN;
                }
        }


        size = mul_size(NAMEDATALEN, ash_max_entries);
        AshEntryAppnameBuffer = (char *)
                ShmemInitStruct("Ash Entry AppName Buffer", size, &found);

	if (!found)
        {
                MemSet(AshEntryAppnameBuffer, 0, size);

                /* Initialize Appname pointers. */
                buffer = AshEntryAppnameBuffer;
                for (i = 0; i < ash_max_entries; i++)
			                {
                        AshEntryArray[i].application_name = buffer;
                        buffer += NAMEDATALEN;
                }
        }

	size = mul_size(NAMEDATALEN, ash_max_entries);
        AshEntryClientaddrBuffer = (char *)
                ShmemInitStruct("Ash Entry Client Addr Buffer", size, &found);

        if (!found)
        {
                MemSet(AshEntryClientaddrBuffer, 0, size);

                /* Initialize Client Addr pointers. */
                buffer = AshEntryClientaddrBuffer;
                for (i = 0; i < ash_max_entries; i++)
                                        {
                        AshEntryArray[i].client_addr = buffer;
                        buffer += NAMEDATALEN;
                }
        }


        size = mul_size(NAMEDATALEN, ash_max_entries);
        AshEntryWaitEventTypeBuffer = (char *)
                ShmemInitStruct("Ash Entry Wait event type Buffer", size, &found);

	if (!found)
        {
                MemSet(AshEntryWaitEventTypeBuffer, 0, size);

                /* Initialize wait event type pointers. */
                buffer = AshEntryWaitEventTypeBuffer;
                for (i = 0; i < ash_max_entries; i++)
			                {
                        AshEntryArray[i].wait_event_type = buffer;
                        buffer += NAMEDATALEN;
                }
        }


        size = mul_size(NAMEDATALEN, ash_max_entries);
        AshEntryWaitEventBuffer = (char *)
                ShmemInitStruct("Ash Entry Wait Event Buffer", size, &found);

	if (!found)
        {
                MemSet(AshEntryWaitEventBuffer, 0, size);

                /* Initialize wait event pointers. */
                buffer = AshEntryWaitEventBuffer;
                for (i = 0; i < ash_max_entries; i++)
			                {
                        AshEntryArray[i].wait_event = buffer;
                        buffer += NAMEDATALEN;
                }
        }


        size = mul_size(NAMEDATALEN, ash_max_entries);
        AshEntryStateBuffer = (char *)
                ShmemInitStruct("Ash Entry State Buffer", size, &found);

	if (!found)
        {
                MemSet(AshEntryStateBuffer, 0, size);

                /* Initialize state pointers. */
                buffer = AshEntryStateBuffer;
                for (i = 0; i < ash_max_entries; i++)
			                {
                        AshEntryArray[i].state = buffer;
                        buffer += NAMEDATALEN;
                }
        }

        size = mul_size(NAMEDATALEN, ash_max_entries);
        AshEntryClientHostnameBuffer = (char *)
        ShmemInitStruct("Ash Entry Client Hostname Buffer", size, &found);

        if (!found)
        {
                MemSet(AshEntryClientHostnameBuffer, 0, size);

                /* Initialize client hostname pointers. */
                buffer = AshEntryClientHostnameBuffer;
                for (i = 0; i < ash_max_entries; i++)
                                        {
                        AshEntryArray[i].client_hostname = buffer;
                        buffer += NAMEDATALEN;
                }
        }

        size = mul_size(pgstat_track_activity_query_size, ash_max_entries);
        AshEntryTopLevelQueryBuffer = (char *)
        ShmemInitStruct("Ash Entry Top Level Query Buffer", size, &found);

        if (!found)
        {
                MemSet(AshEntryTopLevelQueryBuffer, 0, size);

                /* Initialize top level query pointers. */
                buffer = AshEntryTopLevelQueryBuffer;
                for (i = 0; i < ash_max_entries; i++)
                                        {
                        AshEntryArray[i].top_level_query = buffer;
                        buffer += pgstat_track_activity_query_size;
                }
        }

	size = mul_size(pgstat_track_activity_query_size, ash_max_entries);
        AshEntryQueryBuffer = (char *)
        ShmemInitStruct("Ash Entry Query Buffer", size, &found);

        if (!found)
        {
                MemSet(AshEntryQueryBuffer, 0, size);

                /* Initialize query pointers. */
                buffer = AshEntryQueryBuffer;
                for (i = 0; i < ash_max_entries; i++)
                                        {
                        AshEntryArray[i].query = buffer;
                        buffer += pgstat_track_activity_query_size;
                }
        }

        size = mul_size(NAMEDATALEN, ash_max_entries);
        AshEntryBackendTypeBuffer = (char *)
        ShmemInitStruct("Ash Entry Backend Type Buffer", size, &found);

        if (!found)
        {
                MemSet(AshEntryBackendTypeBuffer, 0, size);

                /* Initialize backend type pointers. */
                buffer = AshEntryBackendTypeBuffer;
                for (i = 0; i < ash_max_entries; i++)
                                        {
                        AshEntryArray[i].backend_type = buffer;
                        buffer += NAMEDATALEN;
                }
        }

	/*
         * set up a shmem exit hook to do whatever useful (dump to disk later on?).
         */
        if (!IsUnderPostmaster)
                on_shmem_exit(ash_shmem_shutdown, (Datum) 0);


        if (found)
                return;
}

static void
ash_shmem_shutdown(int code, Datum arg)
{
        /* Don't try to dump during a crash. */
        if (code)
                return;

        /* Safety check ... shouldn't get here unless shmem is set up. */
        if (!AshEntryArray)
                return;

	/* dump to disk ?*/
	/* This is the place */
}


static void
pgsentinel_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;
	got_sigterm = true;
#if PG_VERSION_NUM >= 100000
        SetLatch(MyLatch);
#else
        if (MyProc)
                SetLatch(&MyProc->procLatch);
#endif
	errno = save_errno;
}

static void
pgsentinel_sighup(SIGNAL_ARGS)
{
	int save_errno = errno;
	got_sighup = true;
#if PG_VERSION_NUM >= 100000
        SetLatch(MyLatch);
#else
        if (MyProc)
                SetLatch(&MyProc->procLatch);
#endif
	errno = save_errno;
}

static void
ash_entry_store(TimestampTz ash_time, int inserted,const int pid,const char *usename,const int *client_port, Oid *datid, const char *datname, const char *application_name, const char *client_addr, TransactionId *backend_xmin, TimestampTz backend_start, TimestampTz xact_start, TimestampTz query_start, TimestampTz state_change, const char *wait_event_type, const char *wait_event, const char *state, const char *client_hostname, const char *query, const char *backend_type, Oid *usesysid, TransactionId *backend_xid)
{

	        memcpy(AshEntryArray[inserted].usename,usename,Min(strlen(usename)+1,NAMEDATALEN-1));
	        memcpy(AshEntryArray[inserted].datname,datname,Min(strlen(datname)+1,NAMEDATALEN-1));
	        memcpy(AshEntryArray[inserted].application_name,application_name,Min(strlen(application_name)+1,NAMEDATALEN-1));
	        memcpy(AshEntryArray[inserted].wait_event_type,wait_event_type,Min(strlen(wait_event_type)+1,NAMEDATALEN-1));
	        memcpy(AshEntryArray[inserted].wait_event,wait_event,Min(strlen(wait_event)+1,NAMEDATALEN-1));
	        memcpy(AshEntryArray[inserted].state,state,Min(strlen(state)+1,NAMEDATALEN-1));
	        memcpy(AshEntryArray[inserted].client_hostname,client_hostname,Min(strlen(client_hostname)+1,NAMEDATALEN-1));
	        memcpy(AshEntryArray[inserted].top_level_query,query,Min(strlen(query)+1,pgstat_track_activity_query_size-1));
	        memcpy(AshEntryArray[inserted].backend_type,backend_type,Min(strlen(backend_type)+1,NAMEDATALEN-1));
	        memcpy(AshEntryArray[inserted].client_addr,client_addr,Min(strlen(client_addr)+1,NAMEDATALEN-1));
		AshEntryArray[inserted].client_port=client_port;
		AshEntryArray[inserted].datid=datid;
		AshEntryArray[inserted].usesysid=usesysid;
		AshEntryArray[inserted].pid=pid;
		AshEntryArray[inserted].backend_xmin=backend_xmin;
		AshEntryArray[inserted].backend_xid=backend_xid;
		AshEntryArray[inserted].backend_start=backend_start;
		AshEntryArray[inserted].xact_start=xact_start;
		AshEntryArray[inserted].query_start=query_start;
		AshEntryArray[inserted].state_change=state_change;
		procEntry newprocentry = search_procentry(pid);
		AshEntryArray[inserted].queryid=newprocentry.queryid;
		int len = Max(Min(newprocentry.qlen,pgstat_track_activity_query_size-1),1);
	        memcpy(AshEntryArray[inserted].query,newprocentry.query,len);
		AshEntryArray[inserted].query[len]='\0';
		AshEntryArray[inserted].ash_time=ash_time;
}

static void 
ash_prepare_store(TimestampTz ash_time, const int pid, const char* usename,const int *client_port, Oid *datid, const char *datname, const char *application_name, const char *client_addr,TransactionId *backend_xmin, TimestampTz backend_start,TimestampTz xact_start, TimestampTz query_start, TimestampTz state_change, const char *wait_event_type, const char *wait_event, const char *state, const char *client_hostname, const char *query, const char *backend_type, Oid *usesysid, TransactionId *backend_xid)
{
        Assert(pid != NULL);

        /* Safety check... */
        if (!AshEntryArray) { return; }

	inserted = (inserted % ash_max_entries) + 1;
        ash_entry_store(ash_time,inserted - 1,pid,usename,client_port,datid, datname, application_name, client_addr,backend_xmin, backend_start, xact_start, query_start, state_change, wait_event_type, wait_event, state, client_hostname, query, backend_type,usesysid,backend_xid);
}

void
pgsentinel_main(Datum main_arg)
{

	/* Register functions for SIGTERM/SIGHUP management */
	pqsignal(SIGHUP, pgsentinel_sighup);
	pqsignal(SIGTERM, pgsentinel_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to a database */
#if (PG_VERSION_NUM < 110000)
        BackgroundWorkerInitializeConnection(pgsentinelDbName, NULL);
#else
        BackgroundWorkerInitializeConnection(pgsentinelDbName, NULL, 0);
#endif

	while (!got_sigterm)
	{

		int rc, ret, i;

		/* Wait necessary amount of time */
#if PG_VERSION_NUM >= 100000
		rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, ash_sampling_period * 1000L,PG_WAIT_EXTENSION);
                ResetLatch(MyLatch);
#else
		rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, ash_sampling_period * 1000L);
		ResetLatch(&MyProc->procLatch);
#endif

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/* Process signals */
		if (got_sighup)
		{
			/* Process config file */
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
			ereport(LOG, (errmsg("bgworker pgsentinel signal: processed SIGHUP")));
		}

		if (got_sigterm)
		{
			/* Simply exit */
			ereport(LOG, (errmsg("bgworker pgsentinel signal: processed SIGTERM")));
			proc_exit(0);
		}

		MemoryContext uppercxt = CurrentMemoryContext;

        	SetCurrentStatementStartTimestamp();
        	StartTransactionCommand();
        	SPI_connect();
        	PushActiveSnapshot(GetTransactionSnapshot());
        	pgstat_report_activity(STATE_RUNNING, pg_stat_activity_query);

        	/* We can now execute queries via SPI */
        	ret = SPI_execute(pg_stat_activity_query,true, 0);

        	if (ret != SPI_OK_SELECT)
                  elog(FATAL, "cannot select from pg_stat_activity: error code %d", ret);

		/* Do some processing and log stuff disconnected */

                if (SPI_processed > 0)
        	{
		MemoryContext oldcxt = MemoryContextSwitchTo(uppercxt);
		TimestampTz ash_time=GetCurrentTimestamp();
		for (i = 0; i < SPI_processed; i++)
		{
			bool isnull;
			Datum data;
			char *usenamevalue=NULL;
			char *datnamevalue=NULL;
			char *appnamevalue=NULL;
			char *wait_event_typevalue=NULL;
			char *wait_eventvalue=NULL;
			char *client_hostnamevalue=NULL;
			char *queryvalue=NULL;
			char *backend_typevalue=NULL;
			char *statevalue=NULL;
			char *clientaddrvalue=NULL;
			int *pidvalue = NULL;
			int *client_portvalue = NULL;
			Oid *datidvalue = NULL;
			Oid *usesysidvalue = NULL;
			TransactionId *backend_xminvalue = NULL;
			TransactionId *backend_xidvalue = NULL;
			TimestampTz backend_startvalue = NULL;
			TimestampTz xact_startvalue = NULL;
			TimestampTz query_startvalue = NULL;
			TimestampTz state_changevalue = NULL;

			/* Fetch values */

			/* datid */
			data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,1, &isnull);
			if (!isnull) {
                        datidvalue = DatumGetObjectId(data);
                        }

			/* usesysid */
			data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,4, &isnull);
			if (!isnull) {
                        usesysidvalue = DatumGetObjectId(data);
                        }

			/* datname */
			datnamevalue = DatumGetCString(SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,2, &isnull));

			/* pid */
			pidvalue = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,3, &isnull));

			/* client_port */
			client_portvalue = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,9, &isnull));

			/* usename */
			data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,5, &isnull);
                        if (!isnull) {
                        usenamevalue = DatumGetCString(data);
                        }

			/* appname */
		        data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,6, &isnull);
                        if (!isnull) {
                        appnamevalue = TextDatumGetCString(data);
                        }

			/* wait_event_type */
			data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,14, &isnull);
			if (!isnull) {
			wait_event_typevalue = TextDatumGetCString(data);
			}

			/* wait_event */
			data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,15, &isnull);
                        if (!isnull) {
                        wait_eventvalue = TextDatumGetCString(data);
                        }

			/* state */
                        data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,16, &isnull);
                        if (!isnull) {
                        statevalue = TextDatumGetCString(data);
                        }

			/* client_hostname */
			data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,8, &isnull);
                        if (!isnull) {
                        client_hostnamevalue = TextDatumGetCString(data);
                        }

			/* query */
			data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,19, &isnull);
                        if (!isnull) {
                        queryvalue = TextDatumGetCString(data);
                        }

			/* backend_type */
			data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,20, &isnull);
                        if (!isnull) {
                        backend_typevalue = TextDatumGetCString(data);
                        }

			/* client addr */
			data=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,7, &isnull);
                        if (!isnull) {
                        clientaddrvalue = TextDatumGetCString(data);
                        }

			/* backend xid */
			backend_xidvalue = DatumGetTransactionId(SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,17, &isnull));

			/* backedn xmin */
			backend_xminvalue = DatumGetTransactionId(SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,18, &isnull));

			/* backend start */
			backend_startvalue = DatumGetTimestamp(SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,10, &isnull));

			/* xact start */
			xact_startvalue = DatumGetTimestamp(SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,11, &isnull));

			/* query start */
			query_startvalue = DatumGetTimestamp(SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,12, &isnull));

			/* state change */
			state_changevalue = DatumGetTimestamp(SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,13, &isnull));

			/* prepare to store the entry */
			ash_prepare_store(ash_time,pidvalue,usenamevalue ? usenamevalue : "\0",client_portvalue, datidvalue, datnamevalue ? datnamevalue : "\0", appnamevalue ? appnamevalue : "\0",clientaddrvalue ? clientaddrvalue : "\0",backend_xminvalue, backend_startvalue,xact_startvalue,query_startvalue,state_changevalue, wait_event_typevalue ? wait_event_typevalue : "\0", wait_eventvalue ? wait_eventvalue : "\0", statevalue ? statevalue : "\0", client_hostnamevalue ? client_hostnamevalue : "\0",queryvalue ? queryvalue : "\0",backend_typevalue ? backend_typevalue : "\0", usesysidvalue,backend_xidvalue);
		}
		MemoryContextSwitchTo(oldcxt);
		}
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		pgstat_report_activity(STATE_IDLE, NULL);
	}
	/* No problems, so clean exit */
	proc_exit(0);
}

static void
pgsentinel_load_params(void)
{
	DefineCustomIntVariable("pgsentinel_ash.sampling_period",
                            "Duration between each pull (in seconds).",
                            NULL,
                            &ash_sampling_period,
                            1,
                            1,
                            INT_MAX,
                            PGC_SIGHUP,
                            0,
                            NULL,
                            NULL,
                            NULL);

	DefineCustomIntVariable("pgsentinel_ash.max_entries",
                            "Maximum number of ash entries.",
                            NULL,
                            &ash_max_entries,
                            1000,
                            1000,
                            INT_MAX,
                            PGC_SIGHUP,
                            0,
                            NULL,
                            NULL,
                            NULL);

        DefineCustomStringVariable("pgsentinel.db_name",
                	gettext_noop("Database on which the worker connect."),
                	NULL,
                	&pgsentinelDbName,
                	"postgres",
                	PGC_POSTMASTER,
                	GUC_SUPERUSER_ONLY,
                	NULL, NULL, NULL);
}

/*
 * Entry point for worker loading
 */
void
_PG_init(void)
{

	BackgroundWorker worker;

	/* Add parameters */
	pgsentinel_load_params();

        if (!process_shared_preload_libraries_in_progress)
                return;
	
        EmitWarningsOnPlaceholders("Ash Entry Array");
        RequestAddinShmemSpace(ash_entry_memsize());
        RequestNamedLWLockTranche("Ash Entry Array", 1);

        EmitWarningsOnPlaceholders("Proc Entry Array");
        RequestAddinShmemSpace(proc_entry_memsize());
        RequestNamedLWLockTranche("Proc Entry Array", 1);


        /*
         * Install hooks.
         */
        ash_prev_shmem_startup_hook = shmem_startup_hook;
        shmem_startup_hook = ash_shmem_startup;
        prev_post_parse_analyze_hook = post_parse_analyze_hook;
        post_parse_analyze_hook = ash_post_parse_analyze;

	/* Worker parameter and registration */
        memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
#if PG_VERSION_NUM >= 100000
        sprintf(worker.bgw_library_name, "pgsentinel");
        sprintf(worker.bgw_function_name, "pgsentinel_main");
#else
        worker.bgw_main = pgsentinel_main;
#endif
	snprintf(worker.bgw_name, BGW_MAXLEN, "%s", worker_name);
	/* Wait 10 seconds for restart before crash */
	worker.bgw_restart_time = 10;
	worker.bgw_main_arg = (Datum) 0;
#if PG_VERSION_NUM >= 90400
	/*
	 * Notify PID is present since 9.4. If this is not initialized
	 * a static background worker cannot start properly.
	 */
	worker.bgw_notify_pid = 0;
#endif
	RegisterBackgroundWorker(&worker);
}

static void
pg_active_session_history_internal(FunctionCallInfo fcinfo)
{
        ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
        TupleDesc       tupdesc;
        Tuplestorestate *tupstore;
        MemoryContext per_query_ctx;
        MemoryContext oldcontext;

        /* Entry array must exist already */
        if (!AshEntryArray)
                ereport(ERROR,
                                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                                 errmsg("pg_active_session_history must be loaded via shared_preload_libraries")));

        /* check to see if caller supports us returning a tuplestore */
        if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
                ereport(ERROR,
                                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                 errmsg("set-valued function called in context that cannot accept a set")));
        if (!(rsinfo->allowedModes & SFRM_Materialize))
                ereport(ERROR,
                                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                 errmsg("materialize mode required, but it is not " \
                                                "allowed in this context")));

        /* Switch context to construct returned data structures */
        per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
        oldcontext = MemoryContextSwitchTo(per_query_ctx);

        /* Build a tuple descriptor */
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
                elog(ERROR, "return type must be a row type");

        tupstore = tuplestore_begin_heap(true, false, work_mem);
        rsinfo->returnMode = SFRM_Materialize;
        rsinfo->setResult = tupstore;
        rsinfo->setDesc = tupdesc;

        MemoryContextSwitchTo(oldcontext);

	int i =0;
	for (i = 0; i < ash_max_entries; i++)
        {
                Datum           values[PG_ACTIVE_SESSION_HISTORY_COLS];
                bool            nulls[PG_ACTIVE_SESSION_HISTORY_COLS];
                int                     j = 0;

                memset(values, 0, sizeof(values));
                memset(nulls, 0, sizeof(nulls));

		// ash_time
		if (TimestampTzGetDatum(AshEntryArray[i].ash_time))
			values[j++] = TimestampTzGetDatum(AshEntryArray[i].ash_time);
		else
			break;

		// datid
		if (ObjectIdGetDatum(AshEntryArray[i].datid))
			values[j++] = ObjectIdGetDatum(AshEntryArray[i].datid);
		else
			nulls[j++] = true;

		// datname
		if (AshEntryArray[i].datname[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].datname);
                else
                        nulls[j++] = true;

		// pid
		if (Int32GetDatum(AshEntryArray[i].pid))
			values[j++] = Int32GetDatum(AshEntryArray[i].pid);
		else
			nulls[j++] = true;

		// usesysid 
		if (ObjectIdGetDatum(AshEntryArray[i].usesysid))
			values[j++] = ObjectIdGetDatum(AshEntryArray[i].usesysid);
		else
			nulls[j++] = true;

		// usename
		if (AshEntryArray[i].usename[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].usename);
                else
                        nulls[j++] = true;

		// application_name
		if (AshEntryArray[i].application_name[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].application_name);
                else
                        nulls[j++] = true;
		
		// client_addr
                if (AshEntryArray[i].client_addr[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].client_addr);
                else
                        nulls[j++] = true;

		// client_hostname
		if (AshEntryArray[i].client_hostname[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].client_hostname);
                else
                        nulls[j++] = true;

		// client_port
		if (Int32GetDatum(AshEntryArray[i].client_port))
			values[j++] = Int32GetDatum(AshEntryArray[i].client_port);
		else
			nulls[j++] = true;

		// backend_start
		if (TimestampTzGetDatum(AshEntryArray[i].backend_start))
                        values[j++] = TimestampTzGetDatum(AshEntryArray[i].backend_start);
                else
                        nulls[j++] = true;

		// xact_start
		if (TimestampTzGetDatum(AshEntryArray[i].xact_start))
                        values[j++] = TimestampTzGetDatum(AshEntryArray[i].xact_start);
                else
                        nulls[j++] = true;

		// query_start
		if (TimestampTzGetDatum(AshEntryArray[i].query_start))
                        values[j++] = TimestampTzGetDatum(AshEntryArray[i].query_start);
                else
                        nulls[j++] = true;

		// state_change
		if (TimestampTzGetDatum(AshEntryArray[i].state_change))
                        values[j++] = TimestampTzGetDatum(AshEntryArray[i].state_change);
                else
                        nulls[j++] = true;

		// wait_event_type
		if (AshEntryArray[i].wait_event_type[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].wait_event_type);
                else
                        nulls[j++] = true;

		// wait_event
		if (AshEntryArray[i].wait_event[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].wait_event);
                else
                        nulls[j++] = true;

		// state
		if (AshEntryArray[i].state[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].state);
                else
                        nulls[j++] = true;

		// backend_xid
		if (TransactionIdGetDatum(AshEntryArray[i].backend_xid))
                        values[j++] = TransactionIdGetDatum(AshEntryArray[i].backend_xid);
                else
                        nulls[j++] = true;

		// backend_xmin
		if (TransactionIdGetDatum(AshEntryArray[i].backend_xmin))
                        values[j++] = TransactionIdGetDatum(AshEntryArray[i].backend_xmin);
                else
                        nulls[j++] = true;

		// top_level_query
	        if (AshEntryArray[i].top_level_query[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].top_level_query);
                else
                        nulls[j++] = true;

		// query
                if (AshEntryArray[i].query[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].query);
                else
                        nulls[j++] = true;

		// query_id
		values[j++] = Int64GetDatum(AshEntryArray[i].queryid);

		// backend_type
		if (AshEntryArray[i].backend_type[0] != '\0')
                        values[j++] = CStringGetTextDatum(AshEntryArray[i].backend_type);
                else
                        nulls[j++] = true;

                tuplestore_putvalues(tupstore, tupdesc, values, nulls);
        }

        /* clean up and return the tuplestore */
        tuplestore_donestoring(tupstore);
}

Datum
pg_active_session_history(PG_FUNCTION_ARGS)
{
       pg_active_session_history_internal(fcinfo);
       return (Datum) 0;
}

void
_PG_fini(void)
{
        /* Uninstall hooks. */
        shmem_startup_hook = ash_prev_shmem_startup_hook;
	post_parse_analyze_hook = prev_post_parse_analyze_hook;
}
