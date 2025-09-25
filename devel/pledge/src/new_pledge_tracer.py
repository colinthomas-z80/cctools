import subprocess
import sys
import re
from collections import defaultdict
import argparse

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


    def add_access(self, mode, offset=None, length=None):
        self.modes.add(mode)
        if mode == 'read' and offset is not None:
            self.read_offsets.append((offset, length))
            if length is not None:
                self.total_bytes_read += length  
        elif mode == 'write' and offset is not None:
            self.write_offsets.append((offset, length))
        elif mode == 'mmap' and offset is not None:
            self.mmap_access.append((offset, length))
        elif mode == 'getdents64':
            self.getdents = True

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
    # Matches open/openat with multiple access modes, e.g.:
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
    
    newfstatat_re = re.compile(r'newfstatat\(\d+<([^>]+)>,')
    newfstatat_wsize_re = re.compile(
        r'newfstatat\(\d+<([^>]+)>, ".*?", \{[^}]*st_size=(\d+)[^,}]*,'
    )
    for line in strace_lines:
        m = open_re.search(line)
        if m:
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
                    
            continue

        m = fd_file_re.search(line)
        if m:
            fd, filename = m.groups()
            fd_to_file[fd] = filename
            if filename not in file_tree:
                file_tree[filename] = FileAccessNode(filename)
            continue

        m = lseek_re.search(line)
        if m:
            fd, offset, _, result = m.groups()
            fd_offsets[fd] = int(result)
            continue

        m = read_re.search(line)
        if m:
            fd, filename, requested, length = m.groups()
            offset = fd_offsets.get(fd, 0)
            file_tree[filename].add_access('read', offset, int(length))
            file_tree[filename].total_bytes_read += int(length)
            fd_offsets[fd] = offset + int(length)
            continue

        m = write_re.search(line)
        if m:
            fd, filename, requested, length = m.groups()
            offset = fd_offsets.get(fd, 0)
            print(filename)
            file_tree[filename].add_access('write', offset, int(length))
            fd_offsets[fd] = offset + int(length)
            continue

        m = mmap_re.search(line)
        if m:
            _, length, _, fd, offset, filename = m.groups()
            file_tree[filename].add_access('mmap', int(offset), int(length))
            continue

        m = getdents_re.search(line)
        if m:
            fd = m.group(1)
            filename = fd_to_file.get(fd)
            if filename:
                file_tree[filename].add_access('getdents64')
            continue

        m = newfstatat_wsize_re.search(line)
        if m:
            filename, st_size  = m.groups()
            if filename not in file_tree:
                file_tree[filename] = FileAccessNode(filename)
            file_tree[filename].add_access('stat')
            if st_size is not None:
                file_tree[filename].st_size = int(st_size)
            continue

        m = newfstatat_re.search(line)
        if m:
            print("stat caught")
            filename = m.groups()[0]
            if filename not in file_tree:
                file_tree[filename] = FileAccessNode(filename)
            file_tree[filename].add_access('stat')
            continue


    return file_tree

def print_file_tree(file_tree):
    for filename, node in file_tree.items():
        print(f'File: {filename}')
        print(f'  Modes: {", ".join(node.modes)}')
        print(f'  Access Patterns: {", ".join(node.access_pattern())}')
        print(f'  Bytes Read: {node.total_bytes_read}')
        print(f'  Read Offsets: {node.read_offsets}')
        print(f'  Write Offsets: {node.write_offsets}')
        print()

def main():

    parser = argparse.ArgumentParser(description='Trace file access patterns using strace or parse a strace output file.')
    parser.add_argument('target', nargs='?', help='<pid|command> or path to strace output file')
    parser.add_argument('--file', '-f', dest='strace_file', help='Parse strace output from file instead of running strace')
    args = parser.parse_args()

    if args.strace_file:
        with open(args.strace_file, 'r') as f:
            strace_lines = f.readlines()
        file_tree = parse_strace_output(strace_lines)
        print_file_tree(file_tree)
    elif args.target:
        proc = run_strace(args.target)
        try:
            file_tree = parse_strace_output(proc.stderr)
            print_file_tree(file_tree)
        except KeyboardInterrupt:
            proc.terminate()
    else:
        print('Usage: python new_pledge_tracer.py <pid|command> [--file <strace_output_file>]')
        sys.exit(1)

if __name__ == '__main__':
    main()