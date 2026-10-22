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

m = vine.Manager(port=0)
m.tune("subgraph-window", window)


def region_task(region, output):
    t = vine.Task("sleep 1")
    t.set_cores(1)
    t.set_semantic_category(f"region{region}")
    if output:
        t.set_semantic_output()
    return t


# Each region has two output tasks and one other task, and still counts as one result.
for r in range(regions):
    for i in range(3):
        m.submit(region_task(r, output=(i < 2)))

# A cancelled task does not hold back the result of its region.
cancelled = vine.Task("sleep 100")
cancelled.set_cores(1)
cancelled.set_semantic_category("region3")
m.cancel_by_task_id(m.submit(cancelled))

# Unlabeled tasks are not part of any region.
unlabeled = vine.Task("sleep 1")
unlabeled.set_cores(1)
m.submit(unlabeled)


def manager_status():
    return m.status("manager")[0]


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
    m.submit(region_task(0, output=True))
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
