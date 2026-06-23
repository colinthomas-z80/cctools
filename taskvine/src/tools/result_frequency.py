#! /usr/bin/env python

'''Given a set of task ids, read transaction logs and plot completion intervals over runtime.'''


import argparse
import matplotlib.pyplot as plt
from matplotlib import cm
import numpy as np
import sys

category_dict = {}

def parse_args():
    parser = argparse.ArgumentParser(
        description='Plot task completion frequency for one or more transaction logs.'
    )
    parser.add_argument('num_logfiles', type=int, help='Number of logfile/label pairs')
    parser.add_argument(
        'inputs',
        nargs='+',
        help='Logfile paths followed by labels (num_logfiles each)',
    )
    parser.add_argument(
        '--subgraphs',
        action='store_true',
        help='Plot each logfile in a separate subplot',
    )
    parser.add_argument(
        '--output',
        default='result_frequency.png',
        help='Output image path (default: result_frequency.png)',
    )

    args = parser.parse_args()
    expected = args.num_logfiles * 2
    if len(args.inputs) != expected:
        parser.error(
            f'Expected {expected} values after num_logfiles: '
            f'{args.num_logfiles} logfiles then {args.num_logfiles} labels. '
            f'Got {len(args.inputs)}.'
        )

    args.logfiles = args.inputs[:args.num_logfiles]
    args.labels = args.inputs[args.num_logfiles:]
    return args


def compute_event_curve(logfile, task_ids):
    global category_dict
    events = [0]
    starttime = None

    with open(logfile, 'r') as f:
        lines = f.readlines()

    for line in lines:
        if line.startswith('#'):
            continue

        parts = line.split()
        if not parts:
            continue

        if starttime is None:
            if parts[4] == "RUNNING":
                starttime = float(parts[0])
            continue

        donestr = ' '.join(parts[2:5])
        for task_id in task_ids:
            if donestr == f'TASK {task_id} DONE':
                events.append(float(parts[0]) - starttime)
                break

    if starttime is None:
        return np.array([0.0]), np.array([0.0]), 0.0

    endtime = float(lines[-1].split()[0]) - starttime
    events_s = np.array([e / 1000000 for e in events], dtype=float)
    endtime_s = endtime / 1000000

    x = np.linspace(0, endtime_s, 1000)
    y = np.zeros_like(x)

    for i in range(len(events_s) - 1):
        t_start = events_s[i]
        t_end = events_s[i + 1]
        period = t_end - t_start
        if period <= 0:
            continue

        mask = (x >= t_start) & (x <= t_end)
        x_interval = x[mask]
        phase = 2 * np.pi * (x_interval - t_start) / period
        y[mask] = np.sin(phase)

    return x, events_s, y, endtime_s

if __name__ == '__main__':
    args = parse_args()

    task_ids = [7*x for x in [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]]

    if args.subgraphs:
        fig, axes = plt.subplots(args.num_logfiles, 1, figsize=(12, 3 * args.num_logfiles), sharex=True)
        if args.num_logfiles == 1:
            axes = [axes]
        fig.suptitle('1k Genomes Subgraph Completion')
    else:
        fig, ax = plt.subplots(figsize=(12, 6))
        ax.set_title('1k Genomes Subgraph Completion')
        axes = [ax] * args.num_logfiles

    colors = cm.get_cmap('tab10', 10)

    num_events = len(task_ids)

    for idx, (logfile, label) in enumerate(zip(args.logfiles, args.labels)):
        x, events_s, y, endtime_s = compute_event_curve(logfile, task_ids)
        ax = axes[idx]
        color = colors(idx)

        period_uniform = endtime_s / num_events if num_events > 0 else endtime_s
        y_uniform = np.sin(2 * np.pi * x / period_uniform) if period_uniform > 0 else np.zeros_like(x)
        ax.plot(x, y_uniform, linewidth=1.5, color=color, alpha=0.3)

        ax.plot(x, y, linewidth=2, label=label, color=color)
        ax.scatter(events_s, [0.0 for _ in events_s], s=100, zorder=5, color=colors(idx+1))
        ax.grid(True, alpha=0.3)
        ax.axes.get_yaxis().set_visible(False)

        if args.subgraphs:
            ax.set_ylabel(label, rotation=0, labelpad=35, va='center')
            ax.legend(loc='upper right')

    if not args.subgraphs:
        axes[0].legend()

    axes[-1].set_xlabel('Time (s)')

    fig.tight_layout()
    plt.savefig(args.output, dpi=150)
    plt.show()



    

    
            
