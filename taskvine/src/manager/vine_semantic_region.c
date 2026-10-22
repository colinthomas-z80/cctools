/*
Copyright (C) 2026- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include "vine_semantic_region.h"
#include "vine_manager.h"
#include "vine_task.h"
#include "vine_txn_log.h"

#include "debug.h"
#include "hash_table.h"
#include "macros.h"
#include "rmonitor_types.h"
#include "rmsummary.h"
#include "xxmalloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Seconds between checks of worker saturation and idleness. */
#define VINE_REGULATOR_CHECK_INTERVAL 5
/* Consecutive checks with the same outcome needed to change the resource signal. */
#define VINE_REGULATOR_SIGNAL_CHECKS 3
/* Automatic sample interval: this many mean gaps between results, at least the minimum. */
#define VINE_REGULATOR_SAMPLE_GAPS 10
#define VINE_REGULATOR_SAMPLE_MIN 60

struct vine_regulator *vine_regulator_create(void)
{
	struct vine_regulator *r = calloc(1, sizeof(*r));
	r->regions = hash_table_create(0, 0);
	r->work_weighted = 1;
	r->saturation = 0.05;
	r->idle = 0.25;
	r->signal = VINE_RESOURCE_SIGNAL_OPTIMAL;
	r->candidate = VINE_RESOURCE_SIGNAL_OPTIMAL;
	return r;
}

static void region_delete(struct vine_semantic_region *region)
{
	if (!region)
		return;
	free(region->name);
	free(region);
}

void vine_regulator_delete(struct vine_regulator *r)
{
	if (!r)
		return;
	hash_table_clear(r->regions, (void *)region_delete);
	hash_table_delete(r->regions);
	free(r);
}

const char *vine_resource_signal_string(vine_resource_signal_t signal)
{
	switch (signal) {
	case VINE_RESOURCE_SIGNAL_UNDER:
		return "under";
	case VINE_RESOURCE_SIGNAL_OVER:
		return "over";
	case VINE_RESOURCE_SIGNAL_OPTIMAL:
		return "optimal";
	}
	return "optimal";
}

int vine_regulator_regions_in_flight(struct vine_regulator *r)
{
	return r->regions_in_flight;
}

static int regulator_enabled(struct vine_regulator *r)
{
	return r && r->window_target > 0;
}

/* Only standard tasks with a non-default label belong to a region. */
static struct vine_semantic_region *region_of_task(struct vine_regulator *r, struct vine_task *t, int create)
{
	if (!r || t->type != VINE_TASK_TYPE_STANDARD)
		return 0;

	if (!t->semantic_category || !strcmp(t->semantic_category, "default"))
		return 0;

	struct vine_semantic_region *region = hash_table_lookup(r->regions, t->semantic_category);
	if (!region && create) {
		region = calloc(1, sizeof(*region));
		region->name = xxstrdup(t->semantic_category);
		hash_table_insert(r->regions, region->name, region);
	}

	return region;
}

/* Keep the count of regions in flight consistent after any change to a region. */
static void region_update_flight(struct vine_regulator *r, struct vine_semantic_region *region)
{
	int in_flight = region->active && !region->complete && region->tasks_pending > 0;
	if (in_flight != region->in_flight) {
		r->regions_in_flight += in_flight ? 1 : -1;
		region->in_flight = in_flight;
	}
}

static double regulator_sample_interval(struct vine_regulator *r)
{
	if (r->sample_interval > 0)
		return r->sample_interval;

	if (r->results < 2)
		return VINE_REGULATOR_SAMPLE_MIN;

	double mean_gap = ((double)(r->time_last_result - r->time_first_result)) / ONE_SECOND / (r->results - 1);
	return MAX(VINE_REGULATOR_SAMPLE_MIN, VINE_REGULATOR_SAMPLE_GAPS * mean_gap);
}

/* Record a region that produced its result. */
static void region_result(struct vine_manager *q, struct vine_semantic_region *region)
{
	struct vine_regulator *r = q->regulator;

	r->results++;
	r->work_complete += region->work;
	r->time_last_result = region->time_complete;

	double span = 0;
	if (region->time_first_dispatch > 0) {
		span = ((double)(region->time_complete - region->time_first_dispatch)) / ONE_SECOND;
	}

	vine_txn_log_write_subgraph(q, region->name, "COMPLETE", region->work, span);

	/* Samples are measured from the first result, so the startup wait is excluded. */
	if (r->results == 1) {
		r->time_first_result = region->time_complete;
		r->sample_start = region->time_complete;
		return;
	}

	double count = 1;
	if (r->work_weighted && r->work_complete > 0) {
		double mean_work = r->work_complete / r->results;
		count = region->work / mean_work;
	}

	r->sample_count += count;
	r->total_count += count;
}

static void region_check_complete(struct vine_manager *q, struct vine_semantic_region *region)
{
	if (region->complete || region->tasks_pending > 0 || region->outputs_pending > 0 || region->outputs_seen < 1) {
		return;
	}

	region->complete = 1;
	region->time_complete = timestamp_get();

	if (region->outputs_failed > 0) {
		debug(D_VINE, "subgraph %s ended with %d failed output tasks", region->name, region->outputs_failed);
		vine_txn_log_write_subgraph(q, region->name, "FAILED", region->work, 0);
	} else {
		region_result(q, region);
	}
}

void vine_regulator_task_submitted(struct vine_manager *q, struct vine_task *t)
{
	struct vine_regulator *r = q->regulator;
	struct vine_semantic_region *region = region_of_task(r, t, 1);
	if (!region)
		return;

	if (region->complete) {
		notice(D_VINE, "task %d was submitted to subgraph %s after the subgraph produced its result.", t->task_id, region->name);
	}

	region->tasks_pending++;
	if (t->semantic_output) {
		region->outputs_pending++;
		region->outputs_seen++;
	}

	region_update_flight(r, region);
}

void vine_regulator_task_dispatched(struct vine_manager *q, struct vine_task *t)
{
	struct vine_regulator *r = q->regulator;
	struct vine_semantic_region *region = region_of_task(r, t, 0);
	if (!region)
		return;

	if (!region->time_first_dispatch) {
		region->time_first_dispatch = timestamp_get();
	}

	region->active = 1;
	region_update_flight(r, region);
}

void vine_regulator_task_finished(struct vine_manager *q, struct vine_task *t)
{
	struct vine_regulator *r = q->regulator;
	struct vine_semantic_region *region = region_of_task(r, t, 0);
	if (!region)
		return;

	region->tasks_pending = MAX(0, region->tasks_pending - 1);

	if (t->resources_allocated && t->time_workers_execute_last > 0) {
		double cores = MAX(1, t->resources_allocated->cores);
		region->work += cores * ((double)t->time_workers_execute_last) / ONE_SECOND;
	}

	if (t->semantic_output) {
		region->outputs_pending = MAX(0, region->outputs_pending - 1);
		if (t->result != VINE_RESULT_SUCCESS) {
			region->outputs_failed++;
		}
	}

	region_check_complete(q, region);
	region_update_flight(r, region);
}

int vine_regulator_task_eligible(struct vine_manager *q, struct vine_task *t)
{
	struct vine_regulator *r = q->regulator;
	if (!regulator_enabled(r))
		return 1;

	struct vine_semantic_region *region = region_of_task(r, t, 0);
	if (!region)
		return 1;

	/*
	Regions admitted to the window remain eligible until they complete,
	even while they wait for more tasks to be submitted. Only regions with
	tasks pending count toward the window, so a region that is momentarily
	empty does not block the admission of the next one.
	*/
	if (region->active && !region->complete)
		return 1;

	return r->regions_in_flight < r->window;
}

void vine_regulator_blocked_on_capacity(struct vine_manager *q, struct vine_task *t)
{
	struct vine_regulator *r = q->regulator;
	if (!regulator_enabled(r))
		return;

	r->blocked_on_capacity = 1;
}

/* Close the current sample, and widen or narrow the window toward the average rate. */
static void regulator_close_sample(struct vine_manager *q, timestamp_t now)
{
	struct vine_regulator *r = q->regulator;

	double elapsed = ((double)(now - r->time_first_result)) / ONE_SECOND;
	double interval = ((double)(now - r->sample_start)) / ONE_SECOND;

	r->rate_avg = elapsed > 0 ? r->total_count / elapsed : 0;

	double expected = r->rate_avg * interval;
	double deviation = r->sample_count - expected;
	double deadband = sqrt(expected);

	r->last_sample = r->sample_count;
	r->lead_lag += deviation;
	r->last_sample_below = deviation < -deadband;

	if (deviation < -deadband) {
		r->window = MIN(r->window + 1, r->window_target);
	} else if (deviation > deadband) {
		r->window = MAX(r->window - 1, 1);
	}

	debug(D_VINE,
			"subgraph sample: %.2f results in %.0fs, expected %.2f, window %d of %d, %d in flight",
			r->sample_count,
			interval,
			expected,
			r->window,
			r->window_target,
			r->regions_in_flight);

	r->sample_start = now;
	r->sample_count = 0;

	vine_txn_log_write_regulator(q, "SAMPLE");
}

static vine_resource_signal_t regulator_candidate_signal(struct vine_manager *q)
{
	struct vine_regulator *r = q->regulator;

	struct vine_stats s;
	vine_get_stats(q, &s);

	int ready_tasks = s.tasks_waiting > 0;

	/* With no workers at all and work to do, the manager is starved. */
	if (s.total_cores < 1) {
		return ready_tasks ? VINE_RESOURCE_SIGNAL_UNDER : VINE_RESOURCE_SIGNAL_OPTIMAL;
	}

	double committed = s.committed_cores;
	double total = s.total_cores;

	int saturated = r->blocked_on_capacity && committed >= (1 - r->saturation) * total;
	int idle = (total - committed) >= r->idle * total;

	int no_results_yet = r->results < 2;
	int window_open = r->regions_in_flight < r->window;

	if (saturated && (no_results_yet || r->last_sample_below || window_open)) {
		return VINE_RESOURCE_SIGNAL_UNDER;
	}

	/*
	Idle cores while the window is full and may still widen are handled by the
	throughput loop. Otherwise there is not enough eligible work for the workers.
	*/
	if (idle && (r->window >= r->window_target || window_open || !ready_tasks)) {
		return VINE_RESOURCE_SIGNAL_OVER;
	}

	return VINE_RESOURCE_SIGNAL_OPTIMAL;
}

void vine_regulator_update(struct vine_manager *q)
{
	struct vine_regulator *r = q->regulator;
	if (!regulator_enabled(r))
		return;

	timestamp_t now = timestamp_get();
	if (now - r->time_last_check < VINE_REGULATOR_CHECK_INTERVAL * ONE_SECOND)
		return;
	r->time_last_check = now;

	if (r->results > 0 && ((double)(now - r->sample_start)) / ONE_SECOND >= regulator_sample_interval(r)) {
		regulator_close_sample(q, now);
	}

	vine_resource_signal_t candidate = regulator_candidate_signal(q);
	r->blocked_on_capacity = 0;

	if (candidate == r->candidate) {
		r->candidate_checks++;
	} else {
		r->candidate = candidate;
		r->candidate_checks = 1;
	}

	if (r->candidate != r->signal && r->candidate_checks >= VINE_REGULATOR_SIGNAL_CHECKS) {
		debug(D_VINE, "resource signal changed from %s to %s", vine_resource_signal_string(r->signal), vine_resource_signal_string(r->candidate));
		r->signal = r->candidate;
		vine_txn_log_write_regulator(q, "SIGNAL");
	}
}

void vine_regulator_to_jx(struct vine_manager *q, struct jx *j)
{
	struct vine_regulator *r = q->regulator;
	if (!regulator_enabled(r))
		return;

	jx_insert_string(j, "resource_signal", vine_resource_signal_string(r->signal));
	jx_insert_integer(j, "subgraph_window_target", r->window_target);
	jx_insert_integer(j, "subgraph_window", r->window);
	jx_insert_integer(j, "subgraphs_in_flight", r->regions_in_flight);
	jx_insert_integer(j, "subgraphs_complete", r->results);
	jx_insert_double(j, "subgraph_rate_avg", r->rate_avg);
	jx_insert_double(j, "subgraph_last_sample", r->last_sample);
	jx_insert_double(j, "subgraph_lead_lag", r->lead_lag);
}
