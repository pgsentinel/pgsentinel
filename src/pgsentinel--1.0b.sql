-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgsentinel" to load this file. \quit

CREATE FUNCTION pg_active_session_history(
    OUT ash_time timestamptz,
    OUT datid Oid,
    OUT datname text,
    OUT pid integer,
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
    OUT total_time double precision,
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
    OUT blk_write_time double precision
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_statements_history'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- Register a view on the function for ease of use.
CREATE VIEW pg_stat_statements_history AS
  SELECT * FROM pg_stat_statements_history();

GRANT SELECT ON pg_stat_statements_history TO PUBLIC;
