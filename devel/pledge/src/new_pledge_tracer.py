import subprocess
import sys
import re
from collections import defaultdict
import argparse
import os

access_legend = {
    'read': 'R',
    'write': 'W',
    'mmap': 'M',
    'getdents64': 'D',
    'stat': 'M',
    'create': 'C',
}

cwd = None

class FileAccessNode:
    def __init__(self, filename):
        self.filename = filename
        self.modes = set()
        self.read_offsets = []
        self.write_offsets = []
        self.mmap_access = []
        self.getdents = False
        self.total_bytes_read = 0 
        self.st_size = 0
        self.num_reads = 0
        self.num_writes = 0
        self.num_stats = 0
        self.num_mmaps = 0
        self.num_getdents = 0
        self.num_creates = 0


    def add_access(self, mode, offset=None, length=None):
        self.modes.add(mode)
        if mode == 'read' and offset is not None:
            self.read_offsets.append((offset, length))
            if length is not None:
                self.total_bytes_read += length  
            self.num_reads += 1
        elif mode == 'write' and offset is not None:
            self.write_offsets.append((offset, length))
            self.num_writes += 1
        elif mode == 'mmap' and offset is not None:
            self.mmap_access.append((offset, length))
        elif mode == 'getdents64':
            self.getdents = True
        elif mode == 'stat':
            self.num_stats += 1

    def access_pattern(self):
        patterns = []
        if self.read_offsets:
            offsets = [o for o, l in self.read_offsets]
            if self.is_sequential(offsets):
                patterns.append('sequential read')
            else:
                patterns.append('random read')
        if self.write_offsets:
            offsets = [o for o, l in self.write_offsets]
            if self.is_sequential(offsets):
                patterns.append('sequential write')
            else:
                patterns.append('random write')
        if self.mmap_access:
            patterns.append('mmap')
        if self.getdents:
            patterns.append('directory listing')
        return patterns

    @staticmethod
    def is_sequential(offsets):
        if len(offsets) < 2:
            return True
        return all(b == a + 1 for a, b in zip(offsets, offsets[1:]))

def run_strace(pid_or_cmd):
    if pid_or_cmd.isdigit():
        cmd = ['strace', '-f', '-y', '--trace=file,read,write,mmap,getdents64,lseek', '-p', pid_or_cmd]
    else:
        cmd = ['strace', '-f', '-y', '--trace=file,read,write,mmap,getdents64,lseek'] + pid_or_cmd.split()
    proc = subprocess.Popen(cmd, stderr=subprocess.PIPE, text=True)
    return proc

def parse_strace_output(strace_lines):
    file_tree = defaultdict(FileAccessNode)
    fd_to_file = {}
    fd_offsets = defaultdict(int)

    # openat(AT_FDCWD</path>, "file.txt", O_WRONLY|O_CREAT|O_APPEND, 0666) = 3</path/file.txt>
    open_re = re.compile(
        r'open(?:at)?\([^,]*, "?([^"]+)"?, ([^,)]+)(?:, [^)]*)?\).*?= (\d+)<([^>]+)>'
    )
    read_re = re.compile(r'read\((\d+)<([^>]+)>,.*?, (\d+)\) = (\d+)')
    write_re = re.compile(r'write\((\d+)<([^>]+)>,.*?, (\d+)\) = (\d+)')
    #write_re = re.compile(r'write\((\d+),.*?, (\d+)\).*? <.*>.*? ([^ ]+)$')
    mmap_re = re.compile(r'mmap\((.*), (\d+), ([^,]+), .*?, (\d+), (\d+)\).*? <.*>.*? ([^ ]+)$')
    getdents_re = re.compile(r'getdents64\((\d+),')
    fd_file_re = re.compile(r'fd (\d+) is ([^ ]+)')
    lseek_re = re.compile(r'lseek\((\d+), (\d+), ([^)]*)\) *= *(\d+)')
    
    #newfstatat_re = re.compile(r'newfstatat\((?:AT_FDCWD<([^>]+)>|[^,]+), "([^"]+)", ([^,]+), ([^)]+)\) *= *(-?\d+)(?:\s+ENOENT)?')

    # newfstatat_wsize_re = re.compile(
    #     r'newfstatat\(.?<(.*)>,.?\"(.*)",.?(?:{.*.|(?:st_size=(.*)),.*}).?,.*\) ='

    # )

    # newfstatat_wsize_re = re.compile(
    #     r'newfstatat\((?:AT_FDCWD|.?)<(.*)>,.?\"(.*)\".*|(?:st_size=(.*),.*)='

    # )
    newfstatat_wsize_re = re.compile(
        r'newfstatat\((?:AT_FDCWD|.?)<(.*)>,.?\"(.*)\"(?:(?:.*)|(?:st_size=(.*)),.*)='

    )

    stat_or_lstat = re.compile(
        r'l?stat\(\"(.*)\"(?:(?:.*)|(?:st_size=(.*)),.*)='

    )
    

    #    newfstatat_wsize_re = re.compile(
    #     r'newfstatat\((?:\d+<([^>]+)>|AT_FDCWD<([^>]+)>), [^,]+, \{[^}]*st_size=(\d+)[^}]*\}[^)]*\)'
    # )

    for line in strace_lines:
        m = open_re.search(line)
        if m:
            print("openat") 
            print(line)
            requested, flags, fd, filename = m.groups()
            fd_to_file[fd] = filename
            if filename not in file_tree:
                file_tree[filename] = FileAccessNode(filename)
            # Parse access mode from flags
            if 'O_RDONLY' in flags:
                file_tree[filename].add_access('read')
            elif 'O_WRONLY' in flags:
                file_tree[filename].add_access('write')
            elif 'O_RDWR' in flags:
                file_tree[filename].add_access('read')
                file_tree[filename].add_access('write')
            if 'O_CREAT' in flags:
                file_tree[filename].add_access('create')

            if 'AT_FDCWD' in line:
                global cwd
                cwd_re = re.search(r'AT_FDCWD<([^>]+)>', line)
                if cwd_re:
                    global cwd
                    cwd = cwd_re.group(1)
                    
            continue

        m = fd_file_re.search(line)
        if m:
            print("fdinfo")
            fd, filename = m.groups()
            fd_to_file[fd] = filename
            if filename not in file_tree:
                file_tree[filename] = FileAccessNode(filename)
            continue

        m = lseek_re.search(line)
        if m:
            print("lseek")
            fd, offset, _, result = m.groups()
            fd_offsets[fd] = int(result)
            continue

        m = read_re.search(line)
        if m:
            #print("read")
            fd, filename, requested, length = m.groups()
            offset = fd_offsets.get(fd, 0)
            try:
                file_tree[filename].add_access('read', offset, int(length))
                file_tree[filename].total_bytes_read += int(length)
                fd_offsets[fd] = offset + int(length)
            except:
                print(f"Warning: read from unknown file descriptor {fd} ({filename})")
                file_tree[filename] = FileAccessNode(filename)
                file_tree[filename].add_access('read', offset, int(length))
                file_tree[filename].total_bytes_read += int(length)
                fd_offsets[fd] = offset + int(length)
            continue

        m = write_re.search(line)
        if m:
            print("write")
            fd, filename, requested, length = m.groups()
            offset = fd_offsets.get(fd, 0)
            try:
                file_tree[filename].add_access('write', offset, int(length))
            except:
                print(f"Warning: write to unknown file descriptor {fd} ({filename})")
                file_tree[filename] = FileAccessNode(filename)
                file_tree[filename].add_access('write', offset, int(length))
            continue

        m = mmap_re.search(line)
        if m:
            print("mmap")
            _, length, _, fd, offset, filename = m.groups()
            file_tree[filename].add_access('mmap', int(offset), int(length))
            continue

        m = getdents_re.search(line)
        if m:
            print("getdents64")
            fd = m.group(1)
            filename = fd_to_file.get(fd)
            if filename:
                file_tree[filename].add_access('getdents64')
            continue

        m = newfstatat_wsize_re.search(line)
        if m:
            print("newfstatat_size")
            #print(line)
            if len(m.groups()) != 3:
                print("Warning: unexpected newfstatat match groups:", m.groups())
                continue

            # depending on the strace version the first or second group may be None
            at_fdcwd, filename, st_size = m.groups() if m.groups()[0] is not None else (m.groups()[1], None, m.groups()[2])
            #print(filename)
            #print(line)

            #AT_EMPTY_PATH
            if filename is None:
                continue

            if filename not in file_tree:
                file_tree[filename] = FileAccessNode(filename)
            file_tree[filename].add_access('stat')
            if st_size is not None:
                file_tree[filename].st_size = int(st_size)
            continue

        m = stat_or_lstat.search(line)
        if m:
            print("stat or lstat")
            #print(line)
       
            if len(m.groups()) != 2:
                print("Warning: unexpected stat or lstat match groups:", m.groups())
                continue

            # depending on the strace version the first or second group may be None
            filename, st_size = m.groups() 
            #print(filename)
            #print(line)

            #AT_EMPTY_PATH
            if filename is None:
                continue

            if filename not in file_tree:
                file_tree[filename] = FileAccessNode(filename)
            file_tree[filename].add_access('stat')
            if st_size is not None:
                file_tree[filename].st_size = int(st_size)
            continue

    return file_tree
def print_file_contract(file_tree):
    for filename, node in file_tree.items():
        modes = ','.join(sorted(node.modes))
        patterns = ', '.join(node.access_pattern())
        print(f"{modes} {filename}\t{patterns}")

def print_file_tree(file_tree):
    # Build a directory tree: {dirpath: {filename: node}}
    print("Access       <Directory>    Count")
    dir_tree = defaultdict(dict)
    for filename, node in file_tree.items():
        dirpath = os.path.dirname(filename)
        dir_tree[dirpath][filename] = node

    base_list = ['dev', 'proc', 'sys', 'run', 'usr', 'etc']

    mid_list = ['miniconda3']

    # Group paths by their root directory
    root_groups = defaultdict(lambda: defaultdict(dict))
    for dirpath, files in dir_tree.items():
        root = dirpath.split('/')[1] if dirpath.startswith('/') else dirpath.split('/')[0]
        if root in base_list:
            root_groups[root][dirpath] = files

    # First handle base_list paths
    for root in sorted(root_groups.keys()):
        total_files = 0
        mode_counts = defaultdict(int)
        access_patterns = defaultdict(int)
        stat_count = 0
        read_count = 0
        write_count = 0
        for dirpath, files in root_groups[root].items():
            total_files += len(files)
            for fname, node in files.items():
                mode_counts['read'] += node.num_reads
                mode_counts['write'] += node.num_writes
                mode_counts['stat'] += node.num_stats
                mode_counts['mmap'] += node.num_mmaps
                mode_counts['getdents64'] += node.num_getdents
                mode_counts['create'] += node.num_creates
                for access_pattern in node.access_pattern():
                    access_patterns[access_pattern] += 1
        if total_files > 0:
            mode_summary = ', '.join(f"{mode}: {count}" for mode, count in sorted(mode_counts.items()) if count > 0) #+ ', ' + ', '.join(f"{pattern}: {count}" for pattern, count in sorted(access_patterns.items()))
            print(f"{''.join(access_legend[mode] for mode, count in mode_counts.items() if count > 0)} </{root}> ({total_files} files) [{mode_summary}]")

    # Then handle mid_list paths
    for mid in mid_list:
        mid_paths = {dirpath: files for dirpath, files in dir_tree.items() if f'/{mid}' in dirpath}
        if mid_paths:
            total_files = 0
            mode_counts = defaultdict(int)
            access_patterns = defaultdict(int)
            stat_count = 0
            read_count = 0
            write_count = 0
            mid_abs = f'/{mid}'
            for dirpath, files in mid_paths.items():
                total_files += len(files)
                mid_abs = dirpath.split(f'/{mid}')[0] + f'/{mid}'
                for fname, node in files.items():
                    for mode in node.modes:
                        mode_counts[mode] += 1
                    mode_counts['read'] += node.num_reads
                    mode_counts['write'] += node.num_writes
                    mode_counts['stat'] += node.num_stats
                    mode_counts['mmap'] += node.num_mmaps
                    mode_counts['getdents64'] += node.num_getdents
                    mode_counts['create'] += node.num_creates
                    for access_pattern in node.access_pattern():
                        access_patterns[access_pattern] += 1
            mode_summary = ', '.join(f"{mode}: {count}" for mode, count in sorted(mode_counts.items()) if count > 0) #+ ', ' + ', '.join(f"{pattern}: {count}" for pattern, count in sorted(access_patterns.items()))
            print(f"{''.join(access_legend[mode] for mode, count in mode_counts.items() if count > 0)} <{mid_abs}> ({total_files} files) [{mode_summary}]")
            
            # Remove printed paths from tree
            for dirpath in mid_paths.keys():
                dir_tree.pop(dirpath)

    # Handle remaining paths
    # if a directory is a local reference, find the absolute path in the tree and combine them
    local_refs = {dirpath: files for dirpath, files in dir_tree.items() if not dirpath.startswith('/')}
    for dirpath, files in local_refs.items():
        # try to find absolute path using cwd from openat calls
        abs_path = os.path.abspath(os.path.join(cwd if cwd else '/', dirpath))
        if abs_path in dir_tree:
            parent_files = dir_tree[abs_path]
            for fname, node in files.items():
                if fname not in parent_files:
                    parent_files[fname] = node
                else:
                    # merge access modes and counts
                    parent_node = parent_files[fname]
                    parent_node.modes.update(node.modes)
                    parent_node.read_offsets.extend(node.read_offsets)
                    parent_node.write_offsets.extend(node.write_offsets)
                    parent_node.mmap_access.extend(node.mmap_access)
                    parent_node.getdents = parent_node.getdents or node.getdents
                    parent_node.total_bytes_read += node.total_bytes_read
                    parent_node.num_reads += node.num_reads
                    parent_node.num_writes += node.num_writes
                    parent_node.num_stats += node.num_stats
                    parent_node.num_mmaps += node.num_mmaps
                    parent_node.num_getdents += node.num_getdents
                    parent_node.num_creates += node.num_creates
            # Remove local ref from tree
            dir_tree.pop(dirpath)

    # Group remaining paths by root and modes
    root_mode_groups = defaultdict(lambda: defaultdict(list))
    for dirpath in sorted(dir_tree.keys()):
        root = dirpath.split('/')[1] if dirpath.startswith('/') else dirpath.split('/')[0]
        if root not in base_list:
            files = dir_tree[dirpath]
            mode_counts = defaultdict(int)
            for fname, node in files.items():
                mode_counts['read'] += node.num_reads
                mode_counts['write'] += node.num_writes
                mode_counts['stat'] += node.num_stats
                mode_counts['mmap'] += node.num_mmaps
                mode_counts['getdents64'] += node.num_getdents
                mode_counts['create'] += node.num_creates
            
            # Create mode signature for grouping
            mode_sig = ''.join(sorted(mode for mode in mode_counts.keys() if mode_counts[mode] > 0))
            root_mode_groups[root][mode_sig].append((dirpath, files, mode_counts))

    # Print grouped directories
    for root in sorted(root_mode_groups.keys()):
        for mode_sig, dir_groups in root_mode_groups[root].items():
            total_files = 0
            combined_mode_counts = defaultdict(int)
            combined_dirs = []

            for dirpath, files, mode_counts in dir_groups:
                total_files += len(files)
                for mode, count in mode_counts.items():
                    combined_mode_counts[mode] += count
                if dirpath:
                    combined_dirs.append(dirpath)
                else:
                    combined_dirs.append('/')

            mode_summary = ', '.join(f"{mode}: {count}" for mode, count in sorted(combined_mode_counts.items()) if count > 0)
            dir_summary = f"<{root}/>" if len(combined_dirs) > 1 else f"<{combined_dirs[0]}>"
            print(f"{''.join(access_legend[mode] for mode, count in combined_mode_counts.items() if count > 0)} {dir_summary} ({total_files} files) [{mode_summary}]")



def main():

    parser = argparse.ArgumentParser(description='Trace file access patterns using strace or parse a strace output file.')
    parser.add_argument('target', nargs='?', help='Path to executable')
    parser.add_argument('--file', '-f', dest='strace_file', help='Parse strace output from file instead of running strace')
    parser.add_argument('--tree', '-t', dest='print_tree', help='Print the trace output in a reduced tree format', action='store_true')
    args = parser.parse_args()

    if args.strace_file:
        with open(args.strace_file, 'r') as f:
            strace_lines = f.readlines()
        file_tree = parse_strace_output(strace_lines)
        if args.print_tree:
            output_file = args.strace_file + '.contract'
            with open(output_file, 'w') as f:
                sys.stdout = f
                print_file_tree(file_tree)
                sys.stdout = sys.__stdout__
        else:
            print_file_contract(file_tree)
    elif args.target:
        proc = run_strace(args.target)
        try:
            file_tree = parse_strace_output(proc.stderr)
            output_file = str(args.target) + '.contract'
            with open(output_file, 'w') as f:
                sys.stdout = f
                print_file_tree(file_tree)
                sys.stdout = sys.__stdout__
        except KeyboardInterrupt:
            proc.terminate()
    else:
        print('Usage: python new_pledge_tracer.py <pid|command> [--file <strace_output_file>]')
        sys.exit(1)

if __name__ == '__main__':
    main()
