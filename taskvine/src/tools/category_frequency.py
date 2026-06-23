#! /usr/bin/env python

'''Given a set of task ids, read transaction logs and plot completion intervals over runtime.'''


import argparse
import itertools
import matplotlib.pyplot as plt
from matplotlib import cm
import numpy as np
import sys


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
    parser.add_argument(
        '--max-components',
        type=int,
        default=200,
        help='Maximum number of positive-frequency Fourier components to plot (default: 80)',
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


def parse_logfile(logfile):
    category_dict = {}
    with open(logfile, 'r') as f:
        lines = f.readlines()

    for line in lines:
        if line.startswith('#'):
            continue

        parts = line.split()
        if not parts:
            continue

        if parts[4] == "READY":
            category_dict[parts[5]] = category_dict.get(parts[5], []) + [parts[3]]

    print(category_dict)
    return category_dict

def compute_event_curve(logfile, task_ids):
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

    logfile = args.logfiles[0]
    category_dict = parse_logfile(logfile)  # Populate category_dict

    # if category_dict.get('default', None) and len(category_dict) > 1:
    #     del category_dict['default']

    # join all values into one key
    all_v = list(itertools.chain.from_iterable(category_dict.values()))
    category_dict = {"freq": all_v}

    #fig, axes = plt.subplots(len(category_dict.keys())+2, 1, figsize=(12, 3 * (len(category_dict.keys())+2)), sharex=True)
    fig, axes = plt.subplots(len(category_dict.keys()), 1, figsize=(12, 3 * (len(category_dict.keys()))), sharex=True)

    if len(category_dict.keys()) == 1:
        axes = [axes]

    colors = cm.get_cmap('tab10')
    all_sig = []
    xmsk = None
    for idx, (category, task_ids) in enumerate(category_dict.items()):
        ymsk = None
        num_events = len(task_ids)
        x, events_s, y, endtime_s = compute_event_curve(logfile, task_ids)
        all_sig.append(y)
        xmsk = x
        ax = axes[idx]
        color = colors(idx)

        period_uniform = endtime_s / num_events if num_events > 0 else endtime_s
        y_uniform = np.sin(2 * np.pi * x / period_uniform) if period_uniform > 0 else np.zeros_like(x)
        ax.plot(x, y_uniform, linewidth=1.5, color=color, alpha=0.3)
        ax.plot(x, y, linewidth=2, label=category)
        ax.scatter(events_s, [0.0 for _ in events_s], s=100, zorder=5, color=colors(idx+1))
        ax.grid(True, alpha=0.3)
        ax.axes.get_yaxis().set_visible(False)
        # ax.axes.get_xaxis().set_visible(False)

        ax.set_ylabel(category, rotation=0, labelpad=35, va='center')
        ax.legend(loc='upper right')

    # ax = axes[-2]
    # combined = np.add.reduce(all_sig)
    # ax.plot(x, combined, linewidth=1.5, color='gray', alpha=0.3)
    # ax.grid(True, alpha=0.3)
    # ax.axes.get_yaxis().set_visible(False)
    # ax.axes.get_xaxis().set_visible(False)

    # # Plot a one-sided discrete Fourier series (frequency-domain line spectrum).
    # ax = axes[-1]
    # freqs = np.fft.fftfreq(len(combined), d=(1/len(combined)))
    # fft_values = np.fft.fft(combined)
    # magnitude = np.abs(fft_values) / len(combined)  # normalize
    # nyquist_idx = len(freqs) // 2
    # freqs_pos = freqs[:nyquist_idx]
    # magnitude_pos = magnitude[:nyquist_idx]
    # if args.max_components > 0:
    #     freqs_pos = freqs_pos[:args.max_components]
    #     magnitude_pos = magnitude_pos[:args.max_components]

    # markerline, stemlines, baseline = ax.stem(
    #     freqs_pos,
    #     magnitude_pos,
    #     linefmt='C0-',
    #     markerfmt='C0o',
    #     basefmt='k-',
    # )
    # plt.setp(markerline, markersize=4)
    # plt.setp(stemlines, linewidth=1.2)
    # plt.setp(baseline, linewidth=0.8, alpha=0.5)

    # ax.set_xlim(0, 50)#freqs_pos[-1] if len(freqs_pos) > 0 else 1)
    # ax.set_ylabel('Amplitude')
    # ax.set_xlabel('Frequency (Hz)')
    # ax.grid(True, alpha=0.3) 

    fig.tight_layout()
    plt.savefig(args.output, dpi=150)
    plt.show()



    

    
            
