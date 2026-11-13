import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/build-kit-cross.sh'

# Stands in for prospero-win's tools/build_wine_ps5.sh: the key follows key.txt in the checkout; a build records its
# arguments and writes the key into the work folder, as the real script does after a successful build.
FAKE_BUILD = '''#!/bin/sh
set -eu
print=0 work= jobs= ccache=0
while [ $# -gt 0 ]; do
    case $1 in
    --print-input-key) print=1 ;;
    --work) work=$2; shift ;;
    --jobs) jobs=$2; shift ;;
    --ccache) ccache=1 ;;
    --*) shift ;;
    esac
    shift
done
key="key-$(cat key.txt)-ccache$ccache"
if [ $print -eq 1 ]; then echo "$key"; exit 0; fi
mkdir -p "$work"
echo "jobs=$jobs ccache=$ccache tls=${PROSPERO_TLS_ROOT:-} env-ccache=${PROSPERO_CCACHE:-} media=${PROSPERO_MEDIA_ROOT:-} cflags=${CFLAGS:-}" > "$work/args"
[ -z "${PW_FAKE_SLEEP:-}" ] || sleep "$PW_FAKE_SLEEP"
echo "$key" > "$work/input-key"
'''


class BuildKitCrossTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        base = Path(self.directory.name).resolve()
        self.repo, self.work_root = base / 'prospero-win', base / 'kit'
        (self.repo / 'tools').mkdir(parents=True)
        (self.repo / 'tools/build_wine_ps5.sh').write_text(FAKE_BUILD)
        (self.repo / 'key.txt').write_text('one\n')
        self.env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
        self.env.update(GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@t',
                        GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1')
        self.git('init', '-q')
        self.git('add', '.')
        self.git('commit', '-q', '-m', 'one')
        self.commit = self.git('rev-parse', 'HEAD')
        config = base / 'kit-build.conf'
        config.write_text('\n'.join([
            f'PROSPERO_WIN_REPO={self.repo}', f'KIT_WORK_ROOT={self.work_root}', 'WINE_SOURCE=/wine/source',
            'WINE_HOST_TOOLS=/wine/build', 'NATIVE_FOUNDATION=/foundation', 'PRX_FOUNDATION=/prx',
            'TLS_ROOT=/tls/root', '']))
        self.env['PLAYDEN_KIT_CONFIG'] = str(config)
        self.cores = int(subprocess.run(['getconf', '_NPROCESSORS_ONLN'], capture_output=True, text=True).stdout)

    def tearDown(self):
        self.directory.cleanup()

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.repo), *args], env=self.env, check=True, capture_output=True,
                              text=True).stdout.strip()

    def run_script(self, *args, check=True):
        result = subprocess.run(['bash', str(SCRIPT), *args], env=self.env, capture_output=True, text=True)
        if check:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def work_from(self, result):
        return Path(result.stdout.strip().splitlines()[-1].removeprefix('work: '))

    def test_uses_every_core_by_default_and_the_count_given(self):
        work = self.work_from(self.run_script(self.commit))
        self.assertEqual((work / 'args').read_text().split()[0], f'jobs={self.cores}')
        self.assertEqual((work / 'args').read_text().split()[2], 'tls=/tls/root')
        work = self.work_from(self.run_script(self.commit, '--jobs', 'max', '--no-reuse'))
        self.assertEqual((work / 'args').read_text().split()[0], f'jobs={self.cores}')
        work = self.work_from(self.run_script(self.commit, '--jobs', '3', '--no-reuse'))
        self.assertEqual((work / 'args').read_text().split()[0], 'jobs=3')

    def test_refuses_a_bad_job_count(self):
        for value in ('0', 'many', '-2'):
            result = self.run_script(self.commit, '--jobs', value, check=False)
            self.assertNotEqual(result.returncode, 0, value)
        self.assertFalse(self.work_root.exists() and list(self.work_root.glob('wine-ps5-*')))

    def test_reuses_a_finished_build_with_the_same_key(self):
        first = self.work_from(self.run_script(self.commit))
        self.assertEqual(first.name, f'wine-ps5-{self.commit[:7]}-' + first.name.split('-')[-1])
        again = self.run_script(self.commit)
        self.assertIn('reusing', again.stdout)
        self.assertEqual(self.work_from(again), first)
        self.assertEqual(len(list(self.work_root.glob('wine-ps5-*'))), 1)
        # Another input (here ccache, in the real script the toolchain or the tree) builds again, in a new folder.
        cached = self.work_from(self.run_script(self.commit, '--ccache'))
        self.assertNotEqual(cached, first)
        self.assertEqual((cached / 'args').read_text().split()[1], 'ccache=1')
        self.assertTrue(cached.name.endswith('.2'))
        # A new commit has another key.
        (self.repo / 'key.txt').write_text('two\n')
        self.git('commit', '-q', '-am', 'two')
        other = self.work_from(self.run_script(self.git('rev-parse', 'HEAD')))
        self.assertNotIn(other, (first, cached))

    def test_names_the_work_folder_and_refuses_an_existing_one(self):
        work = self.work_from(self.run_script(self.commit, '--name', 'test-1'))
        self.assertEqual(work, self.work_root / 'wine-ps5-test-1')
        result = self.run_script(self.commit, '--name', 'test-1', '--no-reuse', check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('exists', result.stderr)

    def test_ignores_build_settings_left_in_the_environment(self):
        self.env.update(PROSPERO_CCACHE='1', PROSPERO_MEDIA_ROOT='/other/media', PROSPERO_TLS_ROOT='/other/tls',
                        CFLAGS='-O0')
        work = self.work_from(self.run_script(self.commit, '--no-reuse'))
        self.assertEqual((work / 'args').read_text().split()[1:],
                         ['ccache=0', 'tls=/tls/root', 'env-ccache=', 'media=', 'cflags='])

    def test_runs_at_the_same_time_take_different_folders(self):
        self.env['PW_FAKE_SLEEP'] = '1'
        runs = [subprocess.Popen(['bash', str(SCRIPT), self.commit, '--no-reuse'], env=self.env,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(3)]
        works = []
        for run in runs:
            out, err = run.communicate()
            self.assertEqual(run.returncode, 0, err)
            works.append(out.strip().splitlines()[-1])
        self.assertEqual(len(set(works)), 3, works)
        self.assertFalse(list(self.work_root.glob('.lock-*')))

    def test_needs_a_configuration(self):
        self.env['PLAYDEN_KIT_CONFIG'] = str(Path(self.directory.name) / 'absent.conf')
        result = self.run_script(self.commit, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no builder configuration', result.stderr)


if __name__ == '__main__':
    unittest.main()
