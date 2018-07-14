MODULE_big = pgsentinel
OBJS = pgsentinel.o $(WIN32RES)

EXTENSION = pgsentinel
DATA = pgsentinel--1.0b.sql
PGFILEDESC = "pgsentinel - active session history"

LDFLAGS_SL += $(filter -lm, $(LIBS))

REGRESS_OPTS = --temp-config ./pgsentinel.conf
REGRESS = pgsentinel

# Disabled because these tests require "shared_preload_libraries=pgsentinel",
# which typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
