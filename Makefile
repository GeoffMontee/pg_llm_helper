MODULE_big = pg_llm_helper
OBJS = pg_llm_helper.o

EXTENSION = pg_llm_helper
DATA = pg_llm_helper--1.0.sql

# Use pg_config to find PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
