#! /usr/bin/env python

import ndcctools.taskvine as vine
import os
import signal
import subprocess
import sys


def timeout(signum, frame):
    print("subgraphs test did not finish in time")
    sys.exit(1)


signal.signal(signal.SIGALRM, timeout)
signal.alarm(300)

regions = 4
window = 2
work_tasks = 3

m = vine.Manager(port=0)
m.tune("subgraph-window", window)


def labeled_task(command, region):
    t = vine.Task(command)
    t.set_cores(1)
    t.set_semantic_category(f"region{region}")
    return t


# Each region fans out to work tasks and merges their outputs in a final task,
# which produces the result of the region.
def submit_region(region):
    m.set_semantic_concurrency(f"region{region}", work_tasks)

    parts = []
    for i in range(work_tasks):
        part = m.declare_temp()
        t = labeled_task(f"sleep 1; echo {region}.{i} > part", region)
        t.add_output(part, "part")
        m.submit(t)
        parts.append(part)

    t = labeled_task("cat " + " ".join(f"part{i}" for i in range(work_tasks)) + " > result", region)
    for i, part in enumerate(parts):
        t.add_input(part, f"part{i}")
    t.set_semantic_output()
    m.submit(t)


for r in range(regions):
    submit_region(r)

# Unlabeled tasks are not part of any region.
unlabeled = vine.Task("sleep 1")
unlabeled.set_cores(1)
m.submit(unlabeled)


def manager_status():
    return m.status("manager")[0]


# Before any region runs, the window can use k regions of work_tasks cores each.
assert manager_status()["subgraph_window_cores"] == window * work_tasks


worker = subprocess.Popen(["vine_worker", "localhost", str(m.port), "--cores", "4", "--memory", "1000", "--disk", "1000", "--timeout", "60"])

try:
    max_in_flight = 0
    while not m.empty():
        m.wait(1)
        max_in_flight = max(max_in_flight, manager_status()["subgraphs_in_flight"])

    status = manager_status()
    print(status)
    assert status["subgraph_window_target"] == window
    assert status["resource_signal"] in ("under", "optimal", "over")
    assert status["subgraphs_complete"] == regions
    assert max_in_flight <= window

    # A task submitted to a region after its result does not count again.
    m.submit(labeled_task("sleep 1", 0))
    while not m.empty():
        m.wait(1)
    assert manager_status()["subgraphs_complete"] == regions

finally:
    worker.terminate()
    worker.wait()

completed = []
with open(os.path.join(m.logging_directory, "transactions")) as f:
    for line in f:
        parts = line.split()
        if len(parts) > 4 and parts[2] == "SUBGRAPH" and parts[4] == "COMPLETE":
            completed.append(parts[3])

print(completed)
assert sorted(completed) == [f"region{r}" for r in range(regions)]
