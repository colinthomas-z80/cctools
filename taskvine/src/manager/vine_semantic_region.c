/*
Copyright (C) 2026- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include "vine_semantic_region.h"
#include "vine_manager.h"
#include "vine_file.h"
#include "vine_task.h"
#include "vine_txn_log.h"
#include "vine_worker_info.h"

#include "debug.h"
#include "hash_table.h"
#include "macros.h"
#include "rmonitor_types.h"
#include "rmsummary.h"
#include "xxmalloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Seconds between accumulations of core utilization and blocked time. */
#define VINE_REGULATOR_ACCUMULATE_INTERVAL 1
/* Fraction of a signal interval with window tasks waiting for a worker to be under-allocated. */
#define VINE_REGULATOR_UNDER_BLOCKED 0.5
/* Below this fraction of blocked time, idle cores make the manager over-allocated. */
#define VINE_REGULATOR_OVER_BLOCKED 0.1
/* Automatic sample interval: this many mean gaps between results, at least the minimum. */
#define VINE_REGULATOR_SAMPLE_GAPS 10
#define VINE_REGULATOR_SAMPLE_MIN 60
/* Automatic signal interval: the mean region span, within these bounds. */
#define VINE_REGULATOR_SIGNAL_MIN 15
#define VINE_REGULATOR_SIGNAL_MAX 300

struct vine_regulator *vine_regulator_create(void)
{
	struct vine_regulator *r = calloc(1, sizeof(*r));
	r->regions = hash_table_create(0, 0);
	r->work_weighted = 1;
	r->saturation = 0.05;
	r->idle = 0.25;
	r->concurrency = 1;
	r->signal_interval = 0;
	r->signal_confirm = 2;
	r->release = 1;
	r->decided = VINE_RESOURCE_SIGNAL_OPTIMAL;
	r->pending_signal = VINE_RESOURCE_SIGNAL_OPTIMAL;
	r->signal = VINE_RESOURCE_SIGNAL_OPTIMAL;
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

static struct vine_semantic_region *region_lookup(struct vine_regulator *r, const char *name, int create)
{
	struct vine_semantic_region *region = hash_table_lookup(r->regions, name);
	if (!region && create) {
		region = calloc(1, sizeof(*region));
		region->name = xxstrdup(name);
		hash_table_insert(r->regions, region->name, region);
	}

	return region;
}

/* Only standard tasks with a non-default label belong to a region. */
static struct vine_semantic_region *region_of_task(struct vine_regulator *r, struct vine_task *t, int create)
{
	if (!r || t->type != VINE_TASK_TYPE_STANDARD)
		return 0;

	if (!t->semantic_category || !strcmp(t->semantic_category, "default"))
		return 0;

	return region_lookup(r, t->semantic_category, create);
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

/*
A decision averaged over one region span covers a whole cycle of the regions'
changing concurrency (such as a fan-out and its merge), instead of one phase.
*/
double vine_regulator_signal_interval(struct vine_regulator *r)
{
	if (r->signal_interval > 0)
		return r->signal_interval;

	if (r->spans < 1)
		return VINE_REGULATOR_SIGNAL_MIN;

	double mean_span = r->span_total / r->spans;
	return MIN(VINE_REGULATOR_SIGNAL_MAX, MAX(VINE_REGULATOR_SIGNAL_MIN, mean_span));
}

/*
With k regions in flight, each taking its span, a result is due every mean span / k
(Little's law). The spans are taken without the time regions waited for workers, as
waiting lengthens them in proportion to the shortage of workers, and the target would
then follow whatever rate the workers already give.
*/
double vine_regulator_target_gap(struct vine_regulator *r)
{
	if (r->spans < 1 || r->service_total <= 0 || r->window_target < 1)
		return 0;

	return r->service_total / r->spans / r->window_target;
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
		r->span_total += span;
		r->service_total += MAX(0, span - region->time_blocked);
		r->spans++;
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
	region->tasks_submitted++;
	region->cores_submitted += (t->resources_requested && t->resources_requested->cores > 0) ? t->resources_requested->cores : 1;

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

	struct vine_semantic_region *region = region_of_task(r, t, 0);
	if (region) {
		region->blocked = 1;
	}
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

	/* Without a target yet, falling below the average stands in for falling behind. */
	double target_gap = vine_regulator_target_gap(r);
	if (target_gap > 0) {
		double due = interval / target_gap;
		r->last_sample_behind = r->sample_count < due - sqrt(due);
	} else {
		r->last_sample_behind = r->last_sample_below;
	}

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

/* Add the time since the last accumulation to the averages of this signal interval. */
static void regulator_accumulate(struct vine_manager *q, timestamp_t now)
{
	struct vine_regulator *r = q->regulator;

	double dt = ((double)(now - r->time_last_accumulate)) / ONE_SECOND;
	r->time_last_accumulate = now;

	struct vine_stats s;
	vine_get_stats(q, &s);

	r->acc_time += dt;
	r->acc_busy += s.committed_cores * dt;
	r->acc_total += s.total_cores * dt;
	r->acc_in_flight += r->regions_in_flight * dt;

	if (r->blocked_on_capacity) {
		r->acc_blocked += dt;

		char *name;
		struct vine_semantic_region *region;
		int iteration;
		HASH_TABLE_ITERATE(r->regions, iteration, name, region)
		{
			if (region->blocked && region->time_first_dispatch > 0) {
				region->time_blocked += dt;
			}
			region->blocked = 0;
		}
	}

	if (s.total_cores < 1 && s.tasks_waiting > 0) {
		r->acc_starved += dt;
	}

	r->blocked_on_capacity = 0;
}

/* With no workers at all and work to do for most of the interval, the manager is starved. */
static int regulator_starved(struct vine_regulator *r)
{
	return r->acc_time > 0 && r->acc_starved / r->acc_time >= 0.5;
}

/* Decide the resource signal from the averages of the interval that just ended. */
static vine_resource_signal_t regulator_decide_signal(struct vine_regulator *r)
{
	if (r->acc_time <= 0)
		return r->signal;

	r->utilization = r->acc_total > 0 ? r->acc_busy / r->acc_total : 0;
	r->blocked = r->acc_blocked / r->acc_time;

	double avg_in_flight = r->acc_in_flight / r->acc_time;

	if (regulator_starved(r)) {
		return VINE_RESOURCE_SIGNAL_UNDER;
	}

	if (r->acc_total <= 0) {
		return VINE_RESOURCE_SIGNAL_OPTIMAL;
	}

	int no_results_yet = r->results < 2;
	int window_open = avg_in_flight < r->window;

	int saturated = r->utilization >= 1 - r->saturation && r->blocked >= VINE_REGULATOR_UNDER_BLOCKED;
	int idle = 1 - r->utilization >= r->idle && r->blocked < VINE_REGULATOR_OVER_BLOCKED;

	/*
	Saturated workers are short when results lag the target, which follows from
	the window and the spans, rather than the average, which follows the workers.
	*/
	if (saturated && (no_results_yet || r->last_sample_behind || window_open)) {
		return VINE_RESOURCE_SIGNAL_UNDER;
	}

	/*
	Idle cores while the window is full and may still widen are handled by the
	throughput loop. Otherwise there is not enough eligible work for the workers.
	*/
	if (idle && (r->window >= r->window_target || window_open)) {
		return VINE_RESOURCE_SIGNAL_OVER;
	}

	return VINE_RESOURCE_SIGNAL_OPTIMAL;
}

static int worker_has_temp_files(struct vine_manager *q, struct vine_worker_info *w)
{
	char *cachename;
	void *replica;
	int iteration;
	HASH_TABLE_ITERATE(w->current_files, iteration, cachename, replica)
	{
		struct vine_file *f = hash_table_lookup(q->file_table, cachename);
		if (f && f->type == VINE_TEMP) {
			return 1;
		}
	}
	return 0;
}

/*
When over-allocated, mark a few workers as draining. They receive no new tasks
and are shut down once their current tasks finish. Workers holding temp files
are kept, so that no intermediate data is lost, and the last worker is never drained.
*/
static int regulator_drain_workers(struct vine_manager *q)
{
	struct vine_regulator *r = q->regulator;
	int drained = 0;

	while (drained < r->release) {
		struct vine_worker_info *candidate = 0;
		int workers = 0;

		char *key;
		struct vine_worker_info *w;
		int iteration;
		HASH_TABLE_ITERATE(q->worker_table, iteration, key, w)
		{
			if (w->type != VINE_WORKER_TYPE_WORKER || w->draining)
				continue;

			workers++;

			if (worker_has_temp_files(q, w))
				continue;

			if (!candidate || w->tasks_committed < candidate->tasks_committed) {
				candidate = w;
			}
		}

		if (!candidate || workers <= 1)
			break;

		debug(D_VINE, "draining worker %s (%s) as the manager is over-allocated", candidate->hostname, candidate->addrport);
		candidate->draining = 1;
		drained++;
	}

	return drained;
}

void vine_regulator_update(struct vine_manager *q)
{
	struct vine_regulator *r = q->regulator;
	if (!regulator_enabled(r))
		return;

	timestamp_t now = timestamp_get();

	if (!r->time_last_accumulate) {
		r->time_last_accumulate = now;
		r->interval_start = now;
		return;
	}

	if (now - r->time_last_accumulate < VINE_REGULATOR_ACCUMULATE_INTERVAL * ONE_SECOND)
		return;

	regulator_accumulate(q, now);

	if (r->results > 0 && ((double)(now - r->sample_start)) / ONE_SECOND >= regulator_sample_interval(r)) {
		regulator_close_sample(q, now);
	}

	if (((double)(now - r->interval_start)) / ONE_SECOND < vine_regulator_signal_interval(r))
		return;

	vine_resource_signal_t previous = r->signal;
	r->decided = regulator_decide_signal(r);

	/*
	The reported signal changes once signal_confirm consecutive decisions agree on
	a new value, so a single interval that caught one phase of the regions does not
	add or drain workers. A starved manager is reported at once.
	*/
	if (r->decided == r->signal) {
		r->pending_count = 0;
	} else {
		if (r->decided == r->pending_signal && r->pending_count > 0) {
			r->pending_count++;
		} else {
			r->pending_signal = r->decided;
			r->pending_count = 1;
		}
		if (r->pending_count >= r->signal_confirm || regulator_starved(r)) {
			r->signal = r->decided;
			r->pending_count = 0;
		}
	}

	r->drained = r->signal == VINE_RESOURCE_SIGNAL_OVER ? regulator_drain_workers(q) : 0;

	vine_txn_log_write_regulator(q, "CHECK");

	if (r->signal != previous) {
		debug(D_VINE, "resource signal changed from %s to %s", vine_resource_signal_string(previous), vine_resource_signal_string(r->signal));
		vine_txn_log_write_regulator(q, "SIGNAL");
	}

	r->interval_start = now;
	r->acc_time = 0;
	r->acc_busy = 0;
	r->acc_total = 0;
	r->acc_blocked = 0;
	r->acc_starved = 0;
	r->acc_in_flight = 0;
}

static double region_concurrency_cores(struct vine_regulator *r, struct vine_semantic_region *region)
{
	int concurrency = region->concurrency > 0 ? region->concurrency : r->concurrency;
	double cores_per_task = region->tasks_submitted > 0 ? region->cores_submitted / region->tasks_submitted : 1;
	return concurrency * cores_per_task;
}

double vine_regulator_window_cores(struct vine_regulator *r)
{
	double in_flight_cores = 0;
	double waiting_cores = 0;
	int waiting = 0;

	char *name;
	struct vine_semantic_region *region;
	int iteration;
	HASH_TABLE_ITERATE(r->regions, iteration, name, region)
	{
		if (region->in_flight) {
			in_flight_cores += region_concurrency_cores(r, region);
		} else if (!region->active && !region->complete && region->tasks_pending > 0) {
			waiting_cores += region_concurrency_cores(r, region);
			waiting++;
		}
	}

	int free_slots = MAX(0, r->window - r->regions_in_flight);
	if (waiting > 0) {
		in_flight_cores += MIN(free_slots, waiting) * (waiting_cores / waiting);
	}

	return in_flight_cores;
}

void vine_regulator_set_concurrency(struct vine_regulator *r, const char *name, int tasks)
{
	if (!r || !name)
		return;

	struct vine_semantic_region *region = region_lookup(r, name, 1);
	region->concurrency = MAX(0, tasks);
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
	jx_insert_double(j, "subgraph_window_cores", vine_regulator_window_cores(r));
	jx_insert_double(j, "subgraph_signal_interval", vine_regulator_signal_interval(r));
	jx_insert_double(j, "subgraph_target_gap", vine_regulator_target_gap(r));
}
