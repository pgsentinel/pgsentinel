#!/usr/bin/env bash
set -x -e

export PATH=/usr/local/pgsql/bin:$PATH

cd src
make
sudo env "PATH=$PATH" make install
