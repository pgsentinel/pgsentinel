#!/usr/bin/env bash
set -x -e

if [ "x${PG_VERSION}" = "xHEAD" ]
then
  sudo tail /tmp/postgres.log
else
  sudo tail /var/log/postgresql/postgresql-${PG_VERSION}-main.log
fi
