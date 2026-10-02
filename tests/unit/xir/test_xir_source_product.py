"""Run the real owned source pipeline with independent source expectations."""
from pathlib import Path
import argparse
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    parser.add_argument('stdlib', type=Path)
    parser.add_argument('--case', choices=('tiny', 'retired', 'array'), default='tiny')
    parser.add_argument('--scan', action='store_true')
    args = parser.parse_args()
    fixtures = Path(__file__).resolve().parents[1] / 'fixtures'
    source = ('fn checked<T:Equal>(a:T,b:T) { assertEqual(a,b) }\n'
              'checked(3,3)\nassert(true)\nprint("source-product-ok")\n')
    with tempfile.TemporaryDirectory(prefix='xray-source-product-') as directory:
        work = Path(directory)
        (work / 'product.xr').write_text(source, encoding='utf-8')
        (work / 'product2.xr').write_text(source.replace('source-product-ok', 'other-product-ok'), encoding='utf-8')
        if args.case == 'tiny':
            path, expected = work / 'product.xr', 'tiny'
        elif args.case == 'retired':
            path, expected = fixtures / 'assertion/retired_names_are_ordinary.xr', 'empty'
        else:
            path, expected = fixtures / 'runtime/array_growth_accounting.xr', 'array'
        authority = work if args.case == 'tiny' else path.parent
        command = [str(args.executable.resolve()), str(authority), str(path), str(args.stdlib.resolve()),
                   'accept', str(work / 'actual.c'), expected, 'scan' if args.scan else 'normal']
        result = subprocess.run(command, check=False)
        if result.returncode:
            return result.returncode
        generated = (work / 'actual.c').read_text(encoding='utf-8')
        assert generated and '({' not in generated
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
