import contextlib
import io
import json
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

import seed_worktree

TOOLS = Path(__file__).resolve().parent
SCRIPT = TOOLS / 'seed-worktree.sh'
HAVE_NINJA = shutil.which('ninja') is not None
HAVE_GIT = shutil.which('git') is not None


def deps_file(paths, deps):
    data = bytearray(b'# ninjadeps\n' + struct.pack('<i', 4))
    for index, path in enumerate(paths):
        encoded = path.encode()
        body = encoded + b'\0' * ((4 - len(encoded) % 4) % 4) + struct.pack('<I', ~index & 0xFFFFFFFF)
        data += struct.pack('<I', len(body)) + body
    for output, mtime, inputs in deps:
        body = struct.pack('<iII', output, mtime & 0xFFFFFFFF, mtime >> 32)
        body += b''.join(struct.pack('<i', item) for item in inputs)
        data += struct.pack('<I', len(body) | 0x80000000) + body
    return bytes(data)


def read_deps(data):
    offset = len(b'# ninjadeps\n') + 4
    paths, deps = [], []
    while offset < len(data):
        header = struct.unpack_from('<I', data, offset)[0]
        size = header & 0x7FFFFFFF
        body = data[offset + 4:offset + 4 + size]
        if header & 0x80000000:
            output, low, high = struct.unpack_from('<iII', body)
            inputs = list(struct.unpack_from(f'<{(size - 12) // 4}i', body, 12))
            deps.append((output, low | high << 32, inputs))
        else:
            checksum = struct.unpack_from('<I', body, size - 4)[0]
            paths.append((body[:-4].rstrip(b'\0').decode(), checksum, size % 4))
        offset += 4 + size
    return paths, deps


def git(*arguments, cwd):
    subprocess.run(['git', *arguments], cwd=cwd, check=True, capture_output=True)


def ninja(build, *arguments):
    return subprocess.run(['ninja', '-C', str(build), *arguments], check=True, capture_output=True,
                          text=True).stdout


def plan(build):
    return [line for line in ninja(build, '-n', 'all').splitlines() if 'compile ' in line]


class HashTests(unittest.TestCase):
    def test_hash_matches_the_value_ninja_recorded_for_a_known_command(self):
        self.assertEqual(format(seed_worktree.murmur64a(b'echo seed > out.txt'), 'x'), 'f5e7d6b722ccfc9')

    def test_recorded_hashes_without_leading_zeros_still_match(self):
        command = 'echo seed > out.txt'
        self.assertTrue(seed_worktree.hash_matches(command, 'f5e7d6b722ccfc9'))
        self.assertTrue(seed_worktree.hash_matches(command, '0f5e7d6b722ccfc9'))
        self.assertFalse(seed_worktree.hash_matches(command + ' ', 'f5e7d6b722ccfc9'))

    def test_hash_check_refuses_logs_it_cannot_reproduce(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, '.ninja_log').write_text('# ninja log v6\n0\t1\t5\tout.txt\tdeadbeef\n')
            Path(root, 'commands.json').write_text(json.dumps({'out.txt': 'echo seed > out.txt'}))
            with self.assertRaises(SystemExit), contextlib.redirect_stdout(io.StringIO()):
                seed_worktree.check_hashes(root, os.path.join(root, 'commands.json'))

    def test_hash_check_uses_the_latest_entry_for_each_output(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, '.ninja_log').write_text(
                '# ninja log v6\n0\t1\t5\tout.txt\tdeadbeef\n0\t1\t6\tout.txt\tf5e7d6b722ccfc9\n')
            Path(root, 'commands.json').write_text(json.dumps({'out.txt': 'echo seed > out.txt'}))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                seed_worktree.check_hashes(root, os.path.join(root, 'commands.json'))
            self.assertIn('1 of 1', output.getvalue())


class RelocateTests(unittest.TestCase):
    def test_dependency_paths_move_with_padding_checksums_and_records_intact(self):
        old, new = '/work/main', '/work/tmp/coa-longer-worktree'
        paths = ['src/a.o', old + '/src/a.h', old + '-legacy/src/b.h', '/usr/include/stdio.h']
        original = deps_file(paths, [(0, 123456789012, [1, 2, 3])])
        with tempfile.TemporaryDirectory() as root:
            Path(root, '.ninja_deps').write_bytes(original)
            Path(root, 'files.txt').write_text('')
            with contextlib.redirect_stdout(io.StringIO()):
                seed_worktree.relocate(old, new, root, os.path.join(root, 'files.txt'))
            moved, deps = read_deps(Path(root, '.ninja_deps').read_bytes())
        self.assertEqual([path for path, _, _ in moved],
                         ['src/a.o', new + '/src/a.h', old + '-legacy/src/b.h', '/usr/include/stdio.h'])
        self.assertEqual([check for _, check, _ in moved], [~index & 0xFFFFFFFF for index in range(4)])
        self.assertEqual({padding for _, _, padding in moved}, {0})
        self.assertEqual(deps, [(0, 123456789012, [1, 2, 3])])

    def test_text_files_are_rewritten_with_their_timestamps_kept(self):
        with tempfile.TemporaryDirectory() as root:
            target = Path(root, 'CMakeCache.txt')
            target.write_text('CMAKE_HOME_DIRECTORY:INTERNAL=/work/main\nOTHER=/work/main-legacy/x\n')
            os.utime(target, ns=(1_000_000_000, 1_000_000_000))
            Path(root, '.ninja_deps').write_bytes(deps_file([], []))
            Path(root, 'files.txt').write_text('CMakeCache.txt\n')
            with contextlib.redirect_stdout(io.StringIO()):
                seed_worktree.relocate('/work/main', '/work/wt', root, os.path.join(root, 'files.txt'))
            self.assertEqual(target.read_text(),
                             'CMAKE_HOME_DIRECTORY:INTERNAL=/work/wt\nOTHER=/work/main-legacy/x\n')
            self.assertEqual(target.stat().st_mtime_ns, 1_000_000_000)

    def test_settings_rewrite_only_the_build_directory(self):
        with tempfile.TemporaryDirectory() as root:
            source = Path(root, 'verify-main.json')
            source.write_text(json.dumps({'build_directory': '/work/main/build-main',
                                          'worldserver': '/work/main/build-main/src/server/apps/worldserver',
                                          'worldserver_config': '/srv/etc/worldserver.conf', 'jobs': 2}))
            out = Path(root, 'wt', 'conf', 'verify-all.json')
            seed_worktree.settings(str(source), '/work/main/build-main', '/work/wt/build-main', str(out))
            self.assertEqual(json.loads(out.read_text()), {
                'build_directory': '/work/wt/build-main',
                'worldserver': '/work/wt/build-main/src/server/apps/worldserver',
                'worldserver_config': '/srv/etc/worldserver.conf', 'jobs': 2})


@unittest.skipUnless(HAVE_NINJA and HAVE_GIT, 'needs ninja and git')
class SeededBuildTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.main = self.root / 'main'
        (self.main / 'src').mkdir(parents=True)
        for name in ('a', 'b'):
            (self.main / 'src' / f'{name}.c').write_text(f'{name} source\n')
            (self.main / 'src' / f'{name}.h').write_text(f'{name} header\n')
        git('init', '-q', cwd=self.main)
        git('-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '--allow-empty', '-m', 'init', cwd=self.main)
        git('add', 'src', cwd=self.main)
        git('-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'sources', cwd=self.main)
        self.main_build = self.main / 'build'
        self.main_build.mkdir()
        self.write_manifest(self.main, self.main_build)
        ninja(self.main_build, 'all')
        self.assertEqual(plan(self.main_build), [])
        self.worktree = self.root / 'tmp' / 'coa-worktree'
        git('worktree', 'add', '-q', '--detach', str(self.worktree), 'HEAD', cwd=self.main)

    def write_manifest(self, checkout, build):
        lines = ['rule cc',
                 '  command = sh -c \'cat $in > $out && printf "%s: %s %s\\n" $out $in $hdr > $out.d\'',
                 '  description = compile $out', '  depfile = $out.d', '  deps = gcc']
        for name in ('a', 'b'):
            lines += [f'build {name}.o: cc {checkout}/src/{name}.c', f'  hdr = {checkout}/src/{name}.h']
        lines += ['build all: phony a.o b.o', '']
        (build / 'build.ninja').write_text('\n'.join(lines))

    def seed(self, rehash=True):
        build = self.worktree / 'build'
        shutil.copytree(self.main_build, build, symlinks=True)
        commands = self.root / 'old_commands.json'
        original_log = self.root / 'ninja_log.orig'
        shutil.copy2(build / '.ninja_log', original_log)
        with contextlib.redirect_stdout(io.StringIO()):
            seed_worktree.save_commands(str(self.main_build), str(commands))
            seed_worktree.check_hashes(str(self.main_build), str(commands))
            seed_worktree.restamp(str(self.main), str(self.worktree), 'build')
            files = self.root / 'files.txt'
            files.write_text('build.ninja\n')
            seed_worktree.relocate(str(self.main), str(self.worktree), str(build), str(files))
            if rehash:
                seed_worktree.rehash(str(self.main), str(self.worktree), str(build), str(commands),
                                     str(original_log))
        return build

    def test_a_seeded_worktree_has_no_work_and_reads_its_own_sources(self):
        build = self.seed()
        self.assertEqual(plan(build), [])
        self.assertIn(str(self.worktree / 'src' / 'a.h'), ninja(build, '-t', 'deps', 'a.o'))
        self.assertNotIn(str(self.main), ninja(build, '-t', 'deps').replace(str(self.worktree), ''))

    def test_a_changed_worktree_header_recompiles_only_its_object(self):
        build = self.seed()
        header = self.worktree / 'src' / 'b.h'
        header.write_text('changed header\n')
        os.utime(header, ns=(header.stat().st_atime_ns, header.stat().st_mtime_ns + 10_000_000_000))
        self.assertEqual(plan(build), ['[1/1] compile b.o'])
        self.assertEqual(plan(self.main_build), [])

    def test_relocation_without_rehashing_rebuilds_everything(self):
        build = self.seed(rehash=False)
        self.assertEqual(len(plan(build)), 2)

    def test_restamp_restores_the_source_timestamps_of_unchanged_files(self):
        later = self.main_build.joinpath('a.o').stat().st_mtime_ns + 10_000_000_000
        for name in ('a.c', 'a.h', 'b.c', 'b.h'):
            os.utime(self.worktree / 'src' / name, ns=(later, later))
        with contextlib.redirect_stdout(io.StringIO()):
            seed_worktree.restamp(str(self.main), str(self.worktree), 'build')
        for name in ('a.c', 'a.h', 'b.c', 'b.h'):
            self.assertEqual((self.worktree / 'src' / name).stat().st_mtime_ns,
                             (self.main / 'src' / name).stat().st_mtime_ns)

    def test_restamp_skips_files_that_differ_from_the_source_checkout(self):
        changed = self.worktree / 'src' / 'a.c'
        changed.write_text('branch change\n')
        before = changed.stat().st_mtime_ns
        dirty = self.main / 'src' / 'b.c'
        dirty.write_text('uncommitted change in main\n')
        before_dirty = (self.worktree / 'src' / 'b.c').stat().st_mtime_ns
        with contextlib.redirect_stdout(io.StringIO()):
            seed_worktree.restamp(str(self.main), str(self.worktree), 'build')
        self.assertEqual(changed.stat().st_mtime_ns, before)
        self.assertEqual((self.worktree / 'src' / 'b.c').stat().st_mtime_ns, before_dirty)


@unittest.skipUnless(HAVE_NINJA and HAVE_GIT and shutil.which('cmake') and shutil.which('c++'),
                     'needs ninja, git, cmake and a C++ compiler')
class ScriptEndToEndTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.main = self.root / 'main'
        files = {
            'CMakeLists.txt': 'cmake_minimum_required(VERSION 3.16)\nproject(seed CXX)\n'
                              'file(GLOB_RECURSE MODULE_SOURCES CONFIGURE_DEPENDS modules/*.cpp)\n'
                              'add_executable(worldserver src/main.cpp src/a.cpp src/b.cpp ${MODULE_SOURCES})\n'
                              'target_include_directories(worldserver PRIVATE src)\n',
            '.gitignore': '/build-main/\n/modules/mod-local/\n',
            'src/shared.h': 'int a();\nint b();\nint local();\n',
            'src/main.cpp': '#include "shared.h"\nint main() { return a() + b() + local(); }\n',
            'src/a.cpp': '#include "shared.h"\nint a() { return 0; }\n',
            'src/b.cpp': '#include "shared.h"\nint b() { return 0; }\n',
            'modules/mod-local/local.cpp': 'int local() { return 0; }\n',
        }
        for name, text in files.items():
            path = self.main / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        identity = ['-c', 'user.name=t', '-c', 'user.email=t@t']
        git('init', '-q', '-b', 'main', cwd=self.main)
        git('add', '.', cwd=self.main)
        git(*identity, 'commit', '-q', '-m', 'sources', cwd=self.main)
        git('switch', '-q', '-c', 'feat/seed-change', cwd=self.main)
        (self.main / 'src' / 'a.cpp').write_text('#include "shared.h"\nint a() { return 1 - 1; }\n')
        git(*identity, 'commit', '-q', '-am', 'change a', cwd=self.main)
        git('switch', '-q', 'main', cwd=self.main)
        self.build = self.main / 'build-main'
        subprocess.run(['cmake', '-S', str(self.main), '-B', str(self.build), '-G', 'Ninja'], check=True,
                       capture_output=True)
        ninja(self.build, 'worldserver')

    def snapshot(self):
        return sorted((path.name, path.stat().st_mtime_ns, path.stat().st_size)
                      for path in self.build.iterdir() if path.is_file())

    def run_script(self, *arguments):
        return subprocess.run(['bash', str(SCRIPT), '--source', str(self.main), '--ignore-running', *arguments],
                              capture_output=True, text=True, cwd=self.main)

    def test_seeded_worktree_plans_only_the_branch_changes_and_leaves_the_source_build_alone(self):
        before = self.snapshot()
        worktree = self.root / 'tmp' / 'coa-seed-change'
        result = self.run_script('feat/seed-change', str(worktree))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('compiles:      1 for worldserver', result.stdout)
        planned = (worktree / 'build-main' / 'seed-worktree-plan.txt').read_text().split()
        self.assertEqual(planned, ['CMakeFiles/worldserver.dir/src/a.cpp.o'])
        self.assertTrue((worktree / 'modules' / 'mod-local' / 'local.cpp').is_file())
        self.assertEqual(self.snapshot(), before)
        cache = (worktree / 'build-main' / 'CMakeCache.txt').read_text()
        self.assertIn(f'CMAKE_HOME_DIRECTORY:INTERNAL={worktree}', cache)
        ninja(worktree / 'build-main', 'worldserver')
        self.assertIn('no work to do', ninja(worktree / 'build-main', 'worldserver'))

    def test_a_stale_source_build_is_refused(self):
        (self.main / 'src' / 'b.cpp').write_text('#include "shared.h"\nint b() { return 2 - 2; }\n')
        result = self.run_script('feat/seed-change', str(self.root / 'tmp' / 'coa-stale'))
        self.assertEqual(result.returncode, 1)
        self.assertIn('pending compiles', result.stderr)
        self.assertFalse((self.root / 'tmp' / 'coa-stale').exists())


class GeneratedRevisionTests(unittest.TestCase):
    def test_no_target_shares_the_generated_revision_header_name(self):
        root = TOOLS.parent
        sources = [root / 'src' / 'genrev' / 'CMakeLists.txt', root / 'src' / 'server' / 'apps' / 'CMakeLists.txt',
                   root / 'src' / 'server' / 'game' / 'CMakeLists.txt', root / 'src' / 'tools' / 'CMakeLists.txt']
        pattern = re.compile(r'(add_custom_target|add_dependencies|set_target_properties)\([^)]*\brevision\.h\b')
        for source in sources:
            self.assertIsNone(pattern.search(source.read_text()), source)
        self.assertIn('add_custom_target(generate_revision_h ALL', sources[0].read_text())


@unittest.skipUnless(HAVE_GIT, 'needs git')
class ScriptArgumentTests(unittest.TestCase):
    def run_script(self, *arguments):
        with tempfile.TemporaryDirectory() as root:
            git('init', '-q', cwd=root)
            return subprocess.run(['bash', str(SCRIPT), '--source', root, *arguments,
                                   os.path.join(root, 'never-created')],
                                  capture_output=True, text=True, cwd=root)

    def test_base_branches_require_a_new_branch(self):
        for ref in ('main', 'origin/main'):
            result = self.run_script(ref)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('pass --branch', result.stderr)

    def test_branch_names_follow_the_repository_convention(self):
        for name in ('claude/fix-thing', 'pet-battles', 'feat/'):
            result = self.run_script('--branch', name, 'origin/main')
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('<type>/<short-name>', result.stderr)

    def test_a_valid_branch_name_passes_to_the_build_checks(self):
        result = self.run_script('--branch', 'feat/pet-breeds', 'origin/main')
        self.assertEqual(result.returncode, 1)
        self.assertIn('is not a Ninja build directory', result.stderr)

    def test_help_lists_the_branch_requirement(self):
        result = subprocess.run(['bash', str(SCRIPT), '--help'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn('required when <ref> is main or origin/main', result.stdout)


if __name__ == '__main__':
    unittest.main()
