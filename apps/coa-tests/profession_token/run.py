import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'src/server/coa/CoAProfessionToken.cpp'


def main():
    source = SOURCE.read_text(encoding='utf-8')
    start = source.index('namespace ProfessionToken\n{')
    end = source.index('\nclass item_coa_profession_rune', start)
    block = source[start:end]
    harness = (ROOT / 'apps/coa-tests/profession_token/harness.cpp').read_text(encoding='utf-8')
    harness = harness.replace('// ACTUAL_PROFESSION_TOKEN', block)
    compiler = shutil.which(os.environ.get('CXX', 'c++'))
    assert compiler, 'Enable a C++20 compiler.'
    with tempfile.TemporaryDirectory(prefix='coa-profession-token-') as directory:
        out = Path(directory)
        cpp = out / 'harness.cpp'
        cpp.write_text(harness, encoding='utf-8')
        exe = out / 'regression'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src/server/coa'),
                        str(cpp), '-o', str(exe)], check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=30)
    print('PASS: profession rune eligibility, tiering, affixes, persistence and boss mapping')


if __name__ == '__main__':
    main()
