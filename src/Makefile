ifdef DEB_BUILD_GNU_TYPE
    REGRESS_OPTS =--temp-config=./pgsentinel.conf
else
    REGRESS_OPTS =--temp-config=./pgsentinel.conf --temp-instance=./tmp_check
endif

REGRESS = pgsentinel-test

MODULE_big = pgsentinel
OBJS = $(patsubst %.c,%.o,$(wildcard ./*.c))

ifeq ($(CC),gcc)
    PG_CPPFLAGS = -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-uninitialized -Wno-implicit-fallthrough -Iinclude
else
    PG_CPPFLAGS = -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -Iinclude
endif

EXTRA_CLEAN += $(addprefix ./,*.gcno *.gcda)

EXTENSION = pgsentinel
DATA = $(wildcard $(EXTENSION)*--*.sql)
PGFILEDESC = "pgsentinel - active session history"

LDFLAGS_SL += $(filter -lm, $(LIBS))

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
