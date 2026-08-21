/* pgsentinel--1.4.1--1.4.2e1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pgsentinel UPDATE TO '1.4.2e1'" to load this file. \quit

/*
 * experdb fork 1st revision (e1) on top of upstream v1.4.2.
 *
 * cpu_usage / memory_usage are OUT parameters, so CREATE OR REPLACE cannot
 * change the signature.  Existing installs may carry an older or hand-altered
 * definition of pg_active_session_history(); pin it here so the shared library
 * and the SQL declaration cannot drift apart.
 *
 * NOTE: pgsentinel.so must be replaced with the matching 1.4.2e1 build in the
 * same maintenance window.  A 31-column .so against a 30-column declaration
 * (or a bigint column fed by a float8 build) silently yields garbage values.
 */

DROP VIEW IF EXISTS pg_active_session_history;
DROP FUNCTION IF EXISTS pg_active_session_history();

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
    OUT blocker_state text,
    OUT cpu_usage bigint,         -- cumulative CPU time since backend start, microseconds
    OUT memory_usage bigint,      -- resident set size, bytes (includes shared_buffers pages)
    OUT mem_private_bytes bigint  -- resident private memory, bytes (RSS - shared)
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_active_session_history'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- Register a view on the function for ease of use.
CREATE VIEW pg_active_session_history AS
  SELECT * FROM pg_active_session_history();

GRANT SELECT ON pg_active_session_history TO PUBLIC;
