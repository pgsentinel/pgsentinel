REGRESS_OPTS =--temp-config=./pgsentinel.conf
REGRESS = pgsentinel-test

MODULE_big = pgsentinel
OBJS = $(patsubst %.c,%.o,$(wildcard ./*.c))

ifeq ($(CC),gcc)
    PG_CPPFLAGS = -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-uninitialized -Wno-implicit-fallthrough -Iinclude
else
    PG_CPPFLAGS = -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-implicit-fallthrough -Iinclude
endif

EXTRA_CLEAN += $(addprefix ./,*.gcno *.gcda)

EXTENSION = pgsentinel
DATA = pgsentinel--1.0.sql
PGFILEDESC = "pgsentinel - active session history"

LDFLAGS_SL += $(filter -lm, $(LIBS))

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
