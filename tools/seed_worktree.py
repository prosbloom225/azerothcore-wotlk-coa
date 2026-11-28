import json
import os
import re
import struct
import subprocess
import sys

MULTIPLIER = 0xC6A4A7935BD1E995
MASK = (1 << 64) - 1
NINJA_SEED = 0xDECAFBADDECAFBAD


def murmur64a(data, seed=NINJA_SEED):
    length = len(data)
    value = (seed ^ (length * MULTIPLIER)) & MASK
    offset = 0
    while length - offset >= 8:
        block = int.from_bytes(data[offset:offset + 8], 'little')
        block = (block * MULTIPLIER) & MASK
        block ^= block >> 47
        block = (block * MULTIPLIER) & MASK
        value ^= block
        value = (value * MULTIPLIER) & MASK
        offset += 8
    tail = data[offset:]
    if tail:
        value ^= int.from_bytes(tail, 'little')
        value = (value * MULTIPLIER) & MASK
    value ^= value >> 47
    value = (value * MULTIPLIER) & MASK
    value ^= value >> 47
    return value


def hash_matches(command, recorded):
    return format(murmur64a(command.encode()), '016x') == recorded.rjust(16, '0')


def compdb(build_dir):
    output = subprocess.run(['ninja', '-C', build_dir, '-t', 'compdb'],
                            capture_output=True, check=True).stdout
    return {entry['output']: entry['command'] for entry in json.loads(output)}


def log_entries(path):
    with open(path) as handle:
        for line in handle.read().split('\n'):
            if line and not line.startswith('#'):
                yield line.split('\t')


def path_pattern(old):
    return re.compile(re.escape(old) + r'(?![-\w])')


def keep_mtime(path, write):
    stat = os.stat(path)
    write()
    os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))


def git_blobs(checkout):
    output = subprocess.run(['git', '-C', checkout, 'ls-files', '-s', '-z'],
                            capture_output=True, check=True).stdout.decode()
    blobs = {}
    for record in output.split('\0'):
        if record:
            meta, name = record.split('\t', 1)
            blobs[name] = meta.split()[1]
    return blobs


def changed_files(checkout):
    output = subprocess.run(['git', '-C', checkout, 'diff', '--name-only', '-z'],
                            capture_output=True, check=True).stdout.decode()
    return set(filter(None, output.split('\0')))


def restamp(source, worktree, build_name):
    dirty = changed_files(source) | changed_files(worktree)
    source_blobs = git_blobs(source)
    files = 0
    for name, blob in git_blobs(worktree).items():
        if name in dirty or source_blobs.get(name) != blob:
            continue
        target = os.path.join(worktree, name)
        if os.path.islink(target) or not os.path.exists(target):
            continue
        stat = os.stat(os.path.join(source, name))
        os.utime(target, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        files += 1
    directories = 0
    for root, names, _ in os.walk(worktree):
        if root == worktree:
            names[:] = [n for n in names if n not in ('.git', build_name)]
        names[:] = [n for n in names if n != '.git']
        counterpart = os.path.join(source, os.path.relpath(root, worktree))
        if os.path.isdir(counterpart):
            stat = os.stat(counterpart)
            os.utime(root, ns=(stat.st_atime_ns, stat.st_mtime_ns))
            directories += 1
    print(f'restamped {files} files and {directories} directories')


def relocate(old, new, build_dir, file_list):
    pattern = re.compile(path_pattern(old).pattern.encode())
    rewritten = 0
    with open(file_list) as handle:
        names = [line for line in handle.read().split('\n') if line]
    for name in names:
        path = os.path.join(build_dir, name)
        with open(path, 'rb') as handle:
            data = handle.read()
        updated = pattern.sub(new.encode(), data)
        if updated != data:
            def write():
                with open(path, 'wb') as out:
                    out.write(updated)
            keep_mtime(path, write)
            rewritten += 1
    deps_path = os.path.join(build_dir, '.ninja_deps')
    with open(deps_path, 'rb') as handle:
        data = handle.read()
    header = b'# ninjadeps\n'
    if not data.startswith(header) or struct.unpack_from('<i', data, len(header))[0] != 4:
        sys.exit('unsupported .ninja_deps format')
    offset = len(header) + 4
    output = bytearray(data[:offset])
    paths = 0
    while offset + 4 <= len(data):
        record_header = struct.unpack_from('<I', data, offset)[0]
        size = record_header & 0x7FFFFFFF
        body = data[offset + 4:offset + 4 + size]
        if len(body) < size:
            break
        if record_header & 0x80000000:
            output += data[offset:offset + 4 + size]
        else:
            original = body[:-4].rstrip(b'\0')
            moved = pattern.sub(new.encode(), original)
            paths += moved != original
            record = moved + b'\0' * ((4 - len(moved) % 4) % 4) + body[-4:]
            output += struct.pack('<I', len(record)) + record
        offset += 4 + size

    def write_deps():
        with open(deps_path, 'wb') as out:
            out.write(output)
    keep_mtime(deps_path, write_deps)
    print(f'rewrote {rewritten} build files and {paths} dependency paths')


def save_commands(build_dir, out_path):
    with open(out_path, 'w') as handle:
        json.dump(compdb(build_dir), handle)


def check_hashes(build_dir, commands_path):
    with open(commands_path) as handle:
        commands = json.load(handle)
    latest = {entry[3]: entry[4] for entry in log_entries(os.path.join(build_dir, '.ninja_log'))}
    matched = checked = 0
    for output, recorded in latest.items():
        if output in commands:
            checked += 1
            matched += hash_matches(commands[output], recorded)
    print(f'hash check: {matched} of {checked} recorded commands match')
    if checked == 0 or matched * 2 < checked:
        sys.exit('command hashes do not match this Ninja version; refusing to rewrite .ninja_log')


def rehash(old, new, build_dir, commands_path, original_log):
    with open(commands_path) as handle:
        old_commands = json.load(handle)
    new_commands = compdb(build_dir)
    pattern = path_pattern(old)
    with open(original_log) as handle:
        lines = handle.read().split('\n')
    output = []
    updated = kept = 0
    for line in lines:
        if not line or line.startswith('#'):
            output.append(line)
            continue
        start, end, mtime, path, recorded = line.split('\t')
        moved = pattern.sub(new, path)
        old_command = old_commands.get(path)
        new_command = new_commands.get(moved)
        if old_command is not None and new_command is not None and hash_matches(old_command, recorded):
            recorded = format(murmur64a(new_command.encode()), 'x')
            updated += 1
        else:
            kept += 1
        output.append('\t'.join([start, end, mtime, moved, recorded]))
    log_path = os.path.join(build_dir, '.ninja_log')

    def write():
        with open(log_path, 'w') as out:
            out.write('\n'.join(output))
    keep_mtime(log_path, write)
    print(f'rehashed {updated} log entries, left {kept} unchanged')


def settings(source_file, old_build, new_build, out_path):
    with open(source_file) as handle:
        data = json.load(handle)
    pattern = path_pattern(old_build)
    for key, value in data.items():
        if isinstance(value, str):
            data[key] = pattern.sub(new_build, value)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w') as handle:
        json.dump(data, handle, indent=1)
        handle.write('\n')


COMMANDS = {
    'restamp': restamp,
    'relocate': relocate,
    'save-commands': save_commands,
    'check-hashes': check_hashes,
    'rehash': rehash,
    'settings': settings,
}

if __name__ == '__main__':
    COMMANDS[sys.argv[1]](*sys.argv[2:])
