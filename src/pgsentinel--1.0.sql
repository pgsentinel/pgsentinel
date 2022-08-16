-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgsentinel" to load this file. \quit

CREATE FUNCTION pg_active_session_history(
    OUT ash_time timestamptz,
    OUT datid Oid,
    OUT datname text,
    OUT pid integer,
    OUT leader_pid integer,
    OUT usesysid Oid,
    OUT usename text,
    OUT application_name text,
    OUT client_addr text,
    OUT client_hostname text,
    OUT client_port integer,
    OUT backend_start timestamptz,
    OUT xact_start timestamptz,
    OUT query_start timestamptz,
    OUT state_change timestamptz,
    OUT wait_event_type text,
    OUT wait_event text,
    OUT state text,
    OUT backend_xid xid,
    OUT backend_xmin xid,
    OUT top_level_query text,
    OUT query text,
    OUT cmdtype text,
    OUT queryid bigint,
    OUT backend_type text,
    OUT blockers integer,
    OUT blockerpid integer,
    OUT blocker_state text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_active_session_history'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- Register a view on the function for ease of use.
CREATE VIEW pg_active_session_history AS
  SELECT * FROM pg_active_session_history();

GRANT SELECT ON pg_active_session_history TO PUBLIC;

CREATE FUNCTION pg_stat_statements_history(
    OUT ash_time timestamptz,
    OUT userid Oid,
    OUT dbid Oid,
    OUT queryid bigint,
    OUT calls bigint,
    OUT total_exec_time double precision,
    OUT rows bigint,
    OUT shared_blks_hit bigint,
    OUT shared_blks_read bigint,
    OUT shared_blks_dirtied bigint,
    OUT shared_blks_written bigint,
    OUT local_blks_hit bigint,
    OUT local_blks_read bigint,
    OUT local_blks_dirtied bigint,
    OUT local_blks_written bigint,
    OUT temp_blks_read bigint,
    OUT temp_blks_written bigint,
    OUT blk_read_time double precision,
    OUT blk_write_time double precision,
	OUT plans bigint,
	OUT total_plan_time double precision,
	OUT wal_records bigint,
	OUT wal_fpi bigint,
	OUT wal_bytes numeric
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_statements_history'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- Register a view on the function for ease of use.
CREATE VIEW pg_stat_statements_history AS
  SELECT * FROM pg_stat_statements_history();

GRANT SELECT ON pg_stat_statements_history TO PUBLIC;

CREATE FUNCTION get_parsedinfo(IN int, OUT pid integer, OUT queryid bigint, OUT query text,OUT cmdtype text)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME','get_parsedinfo'
LANGUAGE C STRICT VOLATILE;

CREATE TABLE pg_hist_stat_statements_history
  (
     snap_time           TIMESTAMPTZ,
     ash_time            TIMESTAMPTZ,
     userid              OID,
     dbid                OID,
     queryid             BIGINT,
     calls               BIGINT,
     total_exec_time     DOUBLE PRECISION,
     rows                BIGINT,
     shared_blks_hit     BIGINT,
     shared_blks_read    BIGINT,
     shared_blks_dirtied BIGINT,
     shared_blks_written BIGINT,
     local_blks_hit      BIGINT,
     local_blks_read     BIGINT,
     local_blks_dirtied  BIGINT,
     local_blks_written  BIGINT,
     temp_blks_read      BIGINT,
     temp_blks_written   BIGINT,
     blk_read_time       DOUBLE PRECISION,
     blk_write_time      DOUBLE PRECISION,
     plans               BIGINT,
     total_plan_time     DOUBLE PRECISION,
     wal_records         BIGINT,
     wal_fpi             BIGINT,
     wal_bytes           NUMERIC
  );

CREATE TABLE pg_hist_active_session_history
  (
     snap_time        TIMESTAMPTZ,
     ash_time         TIMESTAMPTZ,
     datid            OID,
     datname          TEXT,
     pid              INTEGER,
     leader_pid       INTEGER,
     usesysid         OID,
     usename          TEXT,
     application_name TEXT,
     client_addr      TEXT,
     client_hostname  TEXT,
     client_port      INTEGER,
     backend_start    TIMESTAMPTZ,
     xact_start       TIMESTAMPTZ,
     query_start      TIMESTAMPTZ,
     state_change     TIMESTAMPTZ,
     wait_event_type  TEXT,
     wait_event       TEXT,
     state            TEXT,
     backend_xid      XID,
     backend_xmin     XID,
     top_level_query  TEXT,
     query            TEXT,
     cmdtype          TEXT,
     queryid          BIGINT,
     backend_type     TEXT,
     blockers         INTEGER,
     blockerpid       INTEGER,
     blocker_state    TEXT
  ); 

SELECT cron.schedule('statements_history_2_disk', '0,30 * * * *', $$insert into pg_hist_stat_statements_historySELECT Now(),
       ash_time,
       userid,
       dbid,
       queryid,
       calls,
       total_exec_time,
       rows,
       shared_blks_hit,
       shared_blks_read,
       shared_blks_dirtied,
       shared_blks_written,
       local_blks_hit,
       local_blks_read,
       local_blks_dirtied,
       local_blks_written,
       temp_blks_read,
       temp_blks_written,
       blk_read_time,
       blk_write_time,
       plans,
       total_plan_time,
       wal_records,
       wal_fpi,
       wal_bytes
FROM   pg_stat_statements_history
WHERE  Trunc(Extract(second FROM ash_time)) % 10 = 0
AND    ash_time > Now() - interval '30 minutes'$$);

SELECT cron.schedule('ash_history_2_disk', '0,30 * * * *', $$insert into pg_hist_active_session_historySELECT Now(),
       ash_time,
       datid,
       datname,
       pid,
       leader_pid,
       usesysid,
       usename,
       application_name,
       client_addr,
       client_hostname,
       client_port,
       backend_start,
       xact_start,
       query_start,
       state_change,
       wait_event_type,
       wait_event,
       state,
       backend_xid,
       backend_xmin,
       top_level_query,
       query,
       cmdtype,
       queryid,
       backend_type,
       blockers,
       blockerpid,
       blocker_state
FROM   pg_active_session_history
WHERE  Trunc(Extract(second FROM ash_time)) % 10 = 0
AND    ash_time > Now() - interval '30 minutes'$$);
