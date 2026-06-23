#!/usr/bin/env python

# plot the runtimes of a set of transaction logs.


if __name__ == "__main__":
    import argparse
    import matplotlib.pyplot as plt
    import numpy as np
    import glob

    parser = argparse.ArgumentParser(description='Plot runtimes from transaction logs.')
    parser.add_argument('cmp_nums', nargs="+", type=int)
    parser.add_argument('log_index', type=int)
    parser.add_argument('log_count', type=int)
    parser.add_argument('--output', default='runtimes.png', help='Output image path (default: runtimes.png)')
    args = parser.parse_args()

    plt.figure(figsize=(10, 6))

    logfiles = []
    
    log_nums = [args.log_index * o for o in range(1, args.log_count)]

    all_runs = []

    for cmp_num in args.cmp_nums:
        num_files = len(glob.glob(f"cmp{cmp_num}_*"))
        if num_files < args.log_count:
            log_files = [f"cmp{cmp_num}_{x}" for x in [args.log_index * o for o in range(1, num_files + 1)]]
        else:
            log_files = [f"cmp{cmp_num}_{x}" for x in log_nums]

        labels = log_nums[0:num_files]

        runtimes = []
        for logfile in log_files:
            with open(logfile + "/transactions", 'r') as f:
                lines = f.readlines()

            running = None
            stop = None
            for line in lines:
                if line.startswith('#'):
                    continue
                parts = line.strip().split()
                
                if "RUNNING" in line:
                    running = float(parts[0])
                    stop = float(lines[-1].strip().split()[0])
                    runtime = (stop - running)/1000000
                    runtimes.append(runtime)
                    break

        all_runs.append((labels, runtimes))
        plt.plot(labels, runtimes)

    for log in glob.glob("cmp_no_reset*"):
        with open(log + "/transactions", 'r') as f:
            lines = f.readlines()

        running = None
        stop = None
        for line in lines:
            if line.startswith('#'):
                continue
            parts = line.strip().split()
            
            if "RUNNING" in line:
                running = float(parts[0])
                stop = float(lines[-1].strip().split()[0])
                runtime = (stop - running)/1000000
                plt.hlines(runtime, log_nums[0], log_nums[-1], colors='red', linestyles='dashed', label='No Reset')
                break
        
    for i, log in enumerate(glob.glob("cmp_small*")):
        with open(log + "/transactions", 'r') as f:
            lines = f.readlines()

        running = None
        stop = None
        for line in lines:
            if line.startswith('#'):
                continue
            parts = line.strip().split()
            
            if "RUNNING" in line:
                running = float(parts[0])
                stop = float(lines[-1].strip().split()[0])
                runtime = (stop - running)/1000000
                plt.hlines(runtime, log_nums[0], log_nums[-1], colors='green', linestyles='dashed', label=f'{i}')
                break
    
    average_run = np.mean([runs for _, runs in all_runs], axis=0)
    plt.plot(log_nums[:len(average_run)], average_run, label='Average')

    plt.xlabel('Schedule  Depth')
    plt.ylabel('Runtime (seconds)')
    plt.title('Running time vs Schedule Depth')
    plt.xticks(log_nums)  # Show every 10th label to avoid clutter
    plt.legend()
    plt.grid(True)
    plt.show()
    plt.savefig(args.output)
    print(f'Runtimes plot saved to {args.output}')