CREATE EXTENSION pg_stat_statements;
CREATE EXTENSION pgsentinel;
select pg_sleep(3);
select count(*) > 0 AS has_data from pg_active_session_history where queryid in (select queryid from pg_stat_statements);
select pg_sleep(3);
select count(*) > 0 AS has_pgssh_data from pg_stat_statements_history;
select 'test2' test2, pg_sleep(3);
select 'test2' test2, pg_sleep(3);
-- Elicit a few seconds where pgsentinel collection is idle.
\! sleep 2

-- Exercise pgssh's limit 2 path when the query disappears from ASH while collection is not idle.
with ash as (
	select queryid, max(ash_time) as max_ash_time
	from pg_active_session_history
	where query like 'select pg_sleep(3)%'
	group by queryid
), pgssh as (
	select queryid, max(ash_time) as max_ash_time
	from pg_stat_statements_history
	group by queryid
)
select pgssh.max_ash_time > ash.max_ash_time as pgssh_after_ash
from ash join pgssh using (queryid);

-- Exercise pgssh's collect_pgssh_on_idle path when the query disappears from ASH while collection is idle.
with ash as (
	select queryid, max(ash_time) as max_ash_time
	from pg_active_session_history
	where query like 'select ''test2'' test2, pg_sleep(3)%'
	group by queryid
), pgssh as (
	select queryid, max(ash_time) as max_ash_time
	from pg_stat_statements_history
	group by queryid
)
select pgssh.max_ash_time > ash.max_ash_time as pgssh_after_ash
from ash join pgssh using (queryid);

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
