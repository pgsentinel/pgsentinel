#!/usr/bin/env bash
set -x -e

sudo su postgres -c "echo 'shared_preload_libraries = '\''pg_stat_statements,pgsentinel'\' >> '${PG_DATADIR}/postgresql.conf'"
sudo su postgres -c "echo 'track_activity_query_size = 2048' >> '${PG_DATADIR}/postgresql.conf'"
sudo su postgres -c "echo 'pg_stat_statements.track = all' >> '${PG_DATADIR}/postgresql.conf'"
