include ../../../config.mk
include ../../../rules.mk

SOURCES = \
	taskvine_json.c \
	vine_manager.c \
	vine_manager_get.c \
	vine_manager_put.c \
	vine_manager_summarize.c \
	vine_schedule.c \
	vine_worker_info.c \
	vine_catalog.c \
	vine_resources.c \
	vine_task.c \
	vine_file.c \
	vine_txn_log.c \
	vine_perf_log.c \
	vine_remote_file_info.c \
	vine_factory_info.c \
	vine_task_info.c \
	vine_blocklist.c

PUBLIC_HEADERS = taskvine.h taskvine_json.h

OBJECTS = $(SOURCES:%.c=%.o)
PUBLIC_HEADERS = taskvine.h taskvine_json.h
LIBRARIES = libtaskvine.a
TARGETS = $(LIBRARIES)

all: $(TARGETS)

libtaskvine.a: $(OBJECTS)

install: all
	mkdir -p $(CCTOOLS_INSTALL_DIR)/lib
	cp $(LIBRARIES) $(CCTOOLS_INSTALL_DIR)/lib/
	mkdir -p $(CCTOOLS_INSTALL_DIR)/include/cctools
	cp $(PUBLIC_HEADERS) $(CCTOOLS_INSTALL_DIR)/include/cctools/

clean:
	rm -rf $(OBJECTS) $(LIBRARIES)

test: all

.PHONY: all clean install test 