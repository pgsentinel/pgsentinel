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

DROP EXTENSION pgsentinel;
DROP EXTENSION pg_stat_statements;
