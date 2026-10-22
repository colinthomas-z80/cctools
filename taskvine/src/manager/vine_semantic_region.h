/*
Copyright (C) 2026- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#ifndef VINE_SEMANTIC_REGION_H
#define VINE_SEMANTIC_REGION_H

/*
Tracking of semantic regions (subgraphs labeled by vine_task_set_semantic_category)
and the regulator that keeps their result throughput steady.

A region produces its result once all of its output tasks (vine_task_set_semantic_output)
are done and no other tasks with its label remain. Each region counts as one result,
regardless of how many output tasks it has.

When the regulator is enabled (tune "subgraph-window" > 0), at most W regions are
admitted for dispatch at a time. Every sample interval the number of results is
compared against the running average rate, and W is widened (below average) or
narrowed (above average) within [1, k]. The manager then reports a resource signal
(under, optimal, over) in its status so that a vine_factory can adjust its workers.
*/

#include "jx.h"
#include "timestamp.h"

struct vine_manager;
struct vine_task;

typedef enum {
	VINE_RESOURCE_SIGNAL_OPTIMAL = 0,
	VINE_RESOURCE_SIGNAL_UNDER,
	VINE_RESOURCE_SIGNAL_OVER,
} vine_resource_signal_t;

struct vine_semantic_region {
	char *name;
	int tasks_pending;   /* Submitted tasks with this label not yet retrieved. */
	int outputs_pending; /* Output tasks with this label not yet retrieved. */
	int outputs_seen;    /* Output tasks with this label ever submitted. */
	int outputs_failed;  /* Output tasks that did not succeed. */
	int active;          /* Admitted to the dispatch window. */
	int complete;        /* Result has been produced (or the region failed). */
	int in_flight;       /* Active, not complete, and with tasks pending. */
	double work;         /* Core-seconds of the tasks retrieved so far. */
	timestamp_t time_first_dispatch;
	timestamp_t time_complete;
};

struct vine_regulator {
	struct hash_table *regions; /* Maps semantic category -> struct vine_semantic_region */

	/* Tunables */
	int window_target;      /* k: target regions in flight. 0 disables the regulator. */
	double sample_interval; /* Seconds per sample. 0 means automatic. */
	int work_weighted;      /* Count results as w_k / mean work instead of 1. */
	double saturation;      /* Cores are saturated when committed >= (1 - saturation) * total. */
	double idle;            /* Cores are idle when free >= idle * total. */

	/* Regulator state */
	int window;           /* W: current number of regions admitted for dispatch. */
	int regions_in_flight;
	int results;          /* Regions that produced their result. */
	double work_complete; /* Total work of the regions that produced their result. */
	timestamp_t time_first_result;
	timestamp_t time_last_result;

	timestamp_t sample_start;
	double sample_count; /* Results (possibly work-weighted) in the current sample. */
	double total_count;  /* Results (possibly work-weighted) since the first result. */
	double last_sample;  /* Results in the last complete sample. */
	double rate_avg;     /* Average results per second since the first result. */
	double lead_lag;     /* Sum of the deviations of each sample from the average. */
	int last_sample_below;

	/* Resource signal */
	timestamp_t time_last_check;
	int blocked_on_capacity; /* A window task found no worker since the last check. */
	vine_resource_signal_t signal;
	vine_resource_signal_t candidate;
	int candidate_checks;
};

struct vine_regulator *vine_regulator_create(void);
void vine_regulator_delete(struct vine_regulator *r);

/* Handle a task state change. Called from change_task_state. */
void vine_regulator_task_submitted(struct vine_manager *q, struct vine_task *t);
void vine_regulator_task_dispatched(struct vine_manager *q, struct vine_task *t);
void vine_regulator_task_finished(struct vine_manager *q, struct vine_task *t);

/* Return true if the task's region may dispatch tasks under the current window. */
int vine_regulator_task_eligible(struct vine_manager *q, struct vine_task *t);

/* Note that an eligible task found no worker with enough free resources. */
void vine_regulator_blocked_on_capacity(struct vine_manager *q, struct vine_task *t);

/* Periodic update of samples, window, and resource signal. */
void vine_regulator_update(struct vine_manager *q);

/* Add regulator fields to a manager status object. */
void vine_regulator_to_jx(struct vine_manager *q, struct jx *j);

int vine_regulator_regions_in_flight(struct vine_regulator *r);
const char *vine_resource_signal_string(vine_resource_signal_t signal);

#endif
