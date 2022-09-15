/*
Copyright (C) 2022- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include "ds_schedule.h"
#include "ds_factory_info.h"
#include "ds_blocklist.h"
#include "ds_file.h"
#include "ds_remote_file_info.h"

#include "debug.h"
#include "rmsummary.h"
#include "rmonitor_types.h"
#include "list.h"
#include "hash_table.h"

#include <limits.h>
#include <math.h>

/*
Check if this task is compatible with this given worker by considering
resources availability, features, blocklist, and all other relevant factors.
Used by all scheduling methods for basic compatibility.
*/

static int check_worker_against_task(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t)
{
	/* worker has not reported any resources yet */
	if(w->resources->tag < 0)
		return 0;

	if(w->resources->workers.total < 1) {
		return 0;
	}

	if(w->draining) {
		return 0;
	}

	if ( w->factory_name ) {
		struct ds_factory_info *f = ds_factory_info_lookup(q,w->factory_name);
		if ( f && f->connected_workers > f->max_workers ) return 0;
	}

	if( ds_blocklist_is_blocked(q,w->hostname) ) {
		return 0;
	}

	struct rmsummary *l = task_worker_box_size(q, w, t);
	struct ds_resources *r = w->resources;

	int ok = 1;

	if(r->disk.inuse + l->disk > r->disk.total) { /* No overcommit disk */
		ok = 0;
	}

	if((l->cores > r->cores.total) || (r->cores.inuse + l->cores > overcommitted_resource_total(q, r->cores.total))) {
		ok = 0;
	}

	if((l->memory > r->memory.total) || (r->memory.inuse + l->memory > overcommitted_resource_total(q, r->memory.total))) {
		ok = 0;
	}

	if((l->gpus > r->gpus.total) || (r->gpus.inuse + l->gpus > overcommitted_resource_total(q, r->gpus.total))) {
		ok = 0;
	}

	//if worker's end time has not been received
	if(w->end_time < 0){
		ok = 0;
	}

	//if wall time for worker is specified and there's not enough time for task, then not ok
	if(w->end_time > 0){
		double current_time = timestamp_get() / ONE_SECOND;
		if(t->resources_requested->end > 0 && w->end_time < t->resources_requested->end) {
			ok = 0;
		}
		if(t->min_running_time > 0 && w->end_time - current_time < t->min_running_time){
			ok = 0;
		}
	}

	rmsummary_delete(l);

	if(t->features) {
		if(!w->features)
			return 0;

		char *feature;
		list_first_item(t->features);
		while((feature = list_next_item(t->features))) {
			if(!hash_table_lookup(w->features, feature))
				return 0;
		}
	}

	return ok;
}

/*
Find the worker that has the largest quantity of cached data needed
by this task, so as to minimize transfer work that must be done
by the manager.
*/

static struct ds_worker_info *find_worker_by_files(struct ds_manager *q, struct ds_task *t)
{
	char *key;
	struct ds_worker_info *w;
	struct ds_worker_info *best_worker = 0;
	int64_t most_task_cached_bytes = 0;
	int64_t task_cached_bytes;
	struct ds_remote_file_info *remote_info;
	struct ds_file *tf;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if( check_worker_against_task(q, w, t) ) {
			task_cached_bytes = 0;
			list_first_item(t->input_files);
			while((tf = list_next_item(t->input_files))) {
				if((tf->type == DS_FILE || tf->type == DS_FILE_PIECE) && (tf->flags & DS_CACHE)) {
					remote_info = hash_table_lookup(w->current_files, tf->cached_name);
					if(remote_info)
						task_cached_bytes += remote_info->size;
				}
			}

			if(!best_worker || task_cached_bytes > most_task_cached_bytes) {
				best_worker = w;
				most_task_cached_bytes = task_cached_bytes;
			}
		}
	}

	return best_worker;
}

/*
Find the first available worker in first-come, first-served order.
Since the order of workers in the hashtable is somewhat arbitrary,
this amounts to simply "find the first available worker".
*/

static struct ds_worker_info *find_worker_by_fcfs(struct ds_manager *q, struct ds_task *t)
{
	char *key;
	struct ds_worker_info *w;
	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void**)&w)) {
		if( check_worker_against_task(q, w, t) ) {
			return w;
		}
	}
	return NULL;
}

/*
Select an available worker at random.
This works by finding all compatible workers,
putting them in a list, and then choosing from the list at random.
*/

static struct ds_worker_info *find_worker_by_random(struct ds_manager *q, struct ds_task *t)
{
	char *key;
	struct ds_worker_info *w = NULL;
	int random_worker;
	struct list *valid_workers = list_create();

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void**)&w)) {
		if(check_worker_against_task(q, w, t)) {
			list_push_tail(valid_workers, w);
		}
	}

	w = NULL;
	if(list_size(valid_workers) > 0) {
		random_worker = (rand() % list_size(valid_workers)) + 1;

		while(random_worker && list_size(valid_workers)) {
			w = list_pop_head(valid_workers);
			random_worker--;
		}
	}

	list_delete(valid_workers);
	return w;
}

// 1 if a < b, 0 if a >= b
static int compare_worst_fit(struct ds_resources *a, struct ds_resources *b)
{
	//Total worker order: free cores > free memory > free disk > free gpus
	if((a->cores.total < b->cores.total))
		return 1;

	if((a->cores.total > b->cores.total))
		return 0;

	//Same number of free cores...
	if((a->memory.total < b->memory.total))
		return 1;

	if((a->memory.total > b->memory.total))
		return 0;

	//Same number of free memory...
	if((a->disk.total < b->disk.total))
		return 1;

	if((a->disk.total > b->disk.total))
		return 0;

	//Same number of free disk...
	if((a->gpus.total < b->gpus.total))
		return 1;

	if((a->gpus.total > b->gpus.total))
		return 0;

	//Number of free resources are the same.
	return 0;
}

/*
Find the worker that is the "worst fit" for this task,
meaning the worker that will have the most resources
unused once this task is placed there.
*/

static struct ds_worker_info *find_worker_by_worst_fit(struct ds_manager *q, struct ds_task *t)
{
	char *key;
	struct ds_worker_info *w;
	struct ds_worker_info *best_worker = NULL;

	struct ds_resources bres;
	struct ds_resources wres;

	memset(&bres, 0, sizeof(struct ds_resources));
	memset(&wres, 0, sizeof(struct ds_resources));

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if( check_worker_against_task(q, w, t) ) {

			//Use total field on bres, wres to indicate free resources.
			wres.cores.total   = w->resources->cores.total   - w->resources->cores.inuse;
			wres.memory.total  = w->resources->memory.total  - w->resources->memory.inuse;
			wres.disk.total    = w->resources->disk.total    - w->resources->disk.inuse;
			wres.gpus.total    = w->resources->gpus.total    - w->resources->gpus.inuse;

			if(!best_worker || compare_worst_fit(&bres, &wres))
			{
				best_worker = w;
				memcpy(&bres, &wres, sizeof(struct ds_resources));
			}
		}
	}

	return best_worker;
}

/*
Find the worker that produced the fastest runtime of prior tasks.
If there are no workers avialable that have previously run a task,
then pick one FCFS.
*/

static struct ds_worker_info *find_worker_by_time(struct ds_manager *q, struct ds_task *t)
{
	char *key;
	struct ds_worker_info *w;
	struct ds_worker_info *best_worker = 0;
	double best_time = HUGE_VAL;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if(check_worker_against_task(q, w, t)) {
			if(w->total_tasks_complete > 0) {
				double t = (w->total_task_time + w->total_transfer_time) / w->total_tasks_complete;
				if(!best_worker || t < best_time) {
					best_worker = w;
					best_time = t;
				}
			}
		}
	}

	if(best_worker) {
		return best_worker;
	} else {
		return find_worker_by_fcfs(q, t);
	}
}

/*
Select the best worker for this task, based on the current scheduling mode.
*/

struct ds_worker_info *ds_schedule_task_to_worker( struct ds_manager *q, struct ds_task *t )
{
	int a = t->worker_selection_algorithm;

	if(a == DS_SCHEDULE_UNSET) {
		a = q->worker_selection_algorithm;
	}

	switch (a) {
	case DS_SCHEDULE_FILES:
		return find_worker_by_files(q, t);
	case DS_SCHEDULE_TIME:
		return find_worker_by_time(q, t);
	case DS_SCHEDULE_WORST:
		return find_worker_by_worst_fit(q, t);
	case DS_SCHEDULE_FCFS:
		return find_worker_by_fcfs(q, t);
	case DS_SCHEDULE_RAND:
	default:
		return find_worker_by_random(q, t);
	}
}

typedef enum {
	CORES_BIT = (1 << 0),
	MEMORY_BIT = (1 << 1),
	DISK_BIT = (1 << 2),
	GPUS_BIT = (1 << 3),
} ds_resource_bitmask_t;


/*
Compares the resources needed by a task to a given worker.
Returns a bitmask that indicates which resource of the task, if any, cannot
be met by the worker. If the task fits in the worker, it returns 0.
*/

static ds_resource_bitmask_t is_task_larger_than_worker(struct ds_manager *q, struct ds_task *t, struct ds_worker_info *w)
{
	if(w->resources->tag < 0) {
		/* quickly return if worker has not sent its resources yet */
		return 0;
	}

	ds_resource_bitmask_t set = 0;
	struct rmsummary *l = task_worker_box_size(q,w,t);

	// baseline resurce comparison of worker total resources and a task requested resorces

	if((double)w->resources->cores.total < l->cores ) {
		set = set | CORES_BIT;
	}

	if((double)w->resources->memory.total < l->memory ) {
		set = set | MEMORY_BIT;
	}

	if((double)w->resources->disk.total < l->disk ) {
		set = set | DISK_BIT;
	}

	if((double)w->resources->gpus.total < l->gpus ) {
		set = set | GPUS_BIT;
	}
	rmsummary_delete(l);

	return set;
}

/*
Compares the resources needed by a task to all connected workers.
Returns 0 if there is worker than can fit the task. Otherwise it returns a bitmask
that indicates that there was at least one worker that could not fit that task resource.
*/

static ds_resource_bitmask_t is_task_larger_than_any_worker( struct ds_manager *q, struct ds_task *t )
{
	char *key;
	struct ds_worker_info *w;
	hash_table_firstkey(q->worker_table);

	int bit_set = 0;
	while(hash_table_nextkey(q->worker_table, &key, (void**)&w))
	{
		ds_resource_bitmask_t new_set = is_task_larger_than_worker(q, t, w);
		if (new_set == 0){
			// Task could run on a currently connected worker, immediately
			// return
			return 0;
		}

		// Inherit the unfit criteria for this task
		bit_set = bit_set | new_set;
	}

	return bit_set;
}

/*
Determine if there exists a ready task that cannot be satisfied
by *any* connected worker, even if all other tasks finish.
If so, then display a suitable message to the user.
This is quite an expensive function and so is invoked only periodically.
*/

void ds_schedule_check_for_large_tasks( struct ds_manager *q )
{
	struct ds_task *t;
	int unfit_core = 0;
	int unfit_mem  = 0;
	int unfit_disk = 0;
	int unfit_gpu  = 0;

	struct rmsummary *largest_unfit_task = rmsummary_create(-1);

	list_first_item(q->ready_list);
	while( (t = list_next_item(q->ready_list))){
		// check each task against the queue of connected workers
		ds_resource_bitmask_t bit_set = is_task_larger_than_any_worker(q,t);
		if(bit_set) {
			rmsummary_merge_max(largest_unfit_task, task_max_resources(q, t));
			rmsummary_merge_max(largest_unfit_task, task_min_resources(q, t));
		}
		if (bit_set & CORES_BIT) {
			unfit_core++;
		}
		if (bit_set & MEMORY_BIT) {
			unfit_mem++;
		}
		if (bit_set & DISK_BIT) {
			unfit_disk++;
		}
		if (bit_set & GPUS_BIT) {
			unfit_gpu++;
		}
	}

	if(unfit_core || unfit_mem || unfit_disk || unfit_gpu){
		notice(D_DS,"There are tasks that cannot fit any currently connected worker:\n");
	}

	if(unfit_core) {
		notice(D_DS,"    %d waiting task(s) need more than %s", unfit_core, rmsummary_resource_to_str("cores", largest_unfit_task->cores, 1));
	}

	if(unfit_mem) {
		notice(D_DS,"    %d waiting task(s) need more than %s of memory", unfit_mem, rmsummary_resource_to_str("memory", largest_unfit_task->memory, 1));
	}

	if(unfit_disk) {
		notice(D_DS,"    %d waiting task(s) need more than %s of disk", unfit_disk, rmsummary_resource_to_str("disk", largest_unfit_task->disk, 1));
	}

	if(unfit_gpu) {
		notice(D_DS,"    %d waiting task(s) need more than %s", unfit_gpu, rmsummary_resource_to_str("gpus", largest_unfit_task->gpus, 1));
	}

	rmsummary_delete(largest_unfit_task);
}