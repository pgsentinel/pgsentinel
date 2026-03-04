CREATE EXTENSION pg_stat_statements;
CREATE EXTENSION pgsentinel;
select pg_sleep(3);
select count(*) > 0 AS has_data from pg_active_session_history where queryid in (select queryid from pg_stat_statements);

begin;
\! sleep 3
commit;

select count(*) > 0 AS has_idle_data from pg_active_session_history where state  = 'idle in transaction';

ALTER SYSTEM SET pgsentinel_ash.track_idle_trans = true;
select pg_reload_conf();

begin;
\! sleep 3
commit;

select count(*) > 0 AS has_idle_data from pg_active_session_history where state  = 'idle in transaction';

-- Test privilege check
CREATE ROLE test_unprivileged LOGIN;

-- Check that unprivileged user sees redacted data for superuser's queries
SET ROLE test_unprivileged;
SELECT bool_or(query = '<insufficient privilege>') AS has_redacted_queries
FROM pg_active_session_history;
RESET ROLE;

DROP ROLE test_unprivileged;

DROP EXTENSION pgsentinel;
DROP EXTENSION pg_stat_statements;
