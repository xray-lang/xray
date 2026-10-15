"""Derive complete selected library traces without executing Xray."""
import csv
import hashlib
import io
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
_INPUTS = {'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'stdlib/csv/csv.xr': '024039cd7c7bb557ddd70b85f0d731dd322795861bc26dfcb94b435a47ac34f8',
 'stdlib/http/http.xr': '5302929606a5dd63aac569a57b6e5c36a6de5eab74ea6727928beaeddb7c4377',
 'tests/diff/cases/semantics/output/print_group_atomic_on_fatal.xr': 'c67eba546c77f41e016e931b9e3d575c3f29ed05012093a92f2994cfa9da1a1d',
 'tests/diff/cases/semantics/slice/_borrow_return_lib.xr': 'e9b5a02cd9c58dcc590008956ae40611c84f545ba8d154e9840cfa6869b84c83',
 'tests/diff/cases/semantics/stdlib/csv_pure_module_direct.xr': 'd669b89d2e9ba351c5afdc0e1d9903611a037949f852e7cd8d7828fb68396791',
 'tests/diff/cases/semantics/stdlib/http_formdata_pure_direct.xr': '4d00ac8a4e993aaf77c4c596f9b445481304a02a54746dd3320a037bcbf7d36f',
 'tests/unit/xir/legacy_consumers/fixtures/execution_ports/print_group_atomic_on_fatal/root.xr': '3f1f979a7ce88aedc09ebcb4229a81b16c16a9429a015b56531734de15277ee1',
 'tests/unit/xir/legacy_consumers/fixtures/library_ports/csv_pure_module_direct/root.xr': '8fdd1078d42cd67daca62d57773603ad117f39e1af6581d563f0abbd2639f1ae',
 'tests/unit/xir/legacy_consumers/fixtures/library_ports/http_formdata_pure_direct/root.xr': 'a56d0a52d66de6de4e045315cec21a28c5b4165e78675037f9aeb8201ef97c86',
 'tests/unit/xir/legacy_consumers/fixtures/slice_unique_export_source/root.xr': 'e9b5a02cd9c58dcc590008956ae40611c84f545ba8d154e9840cfa6869b84c83'}
_PROGRAMS = {'atomic_fatal': ('tests/diff/cases/semantics/output/print_group_atomic_on_fatal.xr',
                  'tests/unit/xir/legacy_consumers/fixtures/execution_ports/print_group_atomic_on_fatal/root.xr'),
 'csv': ('tests/diff/cases/semantics/stdlib/csv_pure_module_direct.xr',
         'tests/unit/xir/legacy_consumers/fixtures/library_ports/csv_pure_module_direct/root.xr'),
 'multipart': ('tests/diff/cases/semantics/stdlib/http_formdata_pure_direct.xr',
               'tests/unit/xir/legacy_consumers/fixtures/library_ports/http_formdata_pure_direct/root.xr'),
 'slice_helper': ('tests/diff/cases/semantics/slice/_borrow_return_lib.xr',
                  'tests/unit/xir/legacy_consumers/fixtures/slice_unique_export_source/root.xr')}


def csv_observations():
    rows = list(csv.reader(io.StringIO('name,age\nAlice,30\nBob,25')))
    header, data = rows[0], rows[1:]
    values = [len(header), header[0], len(data), data[0][0], data[1][1]]
    report = list(csv.reader(io.StringIO('a,b\n1,2\n3')))
    mismatches = [(number, row) for number, row in enumerate(report)
                  if len(row) != len(report[0])]
    record, fields = mismatches[0]
    # Records are zero based; physical lines are one based. At the final
    # unterminated row the completed-field count is the diagnostic column.
    values += [len(report), len(mismatches), len(fields) != len(report[0]),
               record + 1, record, len(fields)]
    bad = list(csv.reader(io.StringIO('a,b\n1')))
    values += [next(i for i, row in enumerate(bad) if len(row) != len(bad[0])) == 1]
    values += [list(csv.reader(io.StringIO('a;b;c\n1;2;3'), delimiter=';'))[1][2],
               list(csv.reader(io.StringIO('x\ty\n4\t5'), delimiter='\t'))[1][1]]
    output = io.StringIO(newline='')
    writer = csv.writer(output, lineterminator='\n')
    writer.writerows([['name', 'note'], ['Alice', 'hello, world'], ['Bob', 'quote " me']])
    text = output.getvalue()
    values += ['"hello, world"' in text, '"quote "" me"' in text,
               list(csv.reader(io.StringIO(text)))[2][1]]
    # A quoted first field is data even when its text begins with '#'.
    records = ['"# value",x', '# ignored', 'y,z']
    comments = [next(csv.reader([line])) for line in records if not line.startswith('#')]
    values += [len(comments), comments[0][0], len(['a', 'b']) > 1]
    return values


def multipart_observations():
    boundary = '----XrayFormBoundary' + 'independent-boundary'
    body = ('--' + boundary + '\r\n'
            'Content-Disposition: form-data; name="name"\r\n\r\n'
            'Xray\r\n--' + boundary + '--\r\n')
    values = [boundary.startswith('----XrayFormBoundary'), True,
              ('multipart/form-data; boundary=' + boundary).startswith(
                  'multipart/form-data; boundary=----XrayFormBoundary'),
              'Content-Disposition: form-data; name="name"\r\n\r\n' in body,
              '\r\n\r\nXray\r\n' in body, body.endswith('--\r\n')]
    file_body = ('--' + boundary + '\r\n'
                 'Content-Disposition: form-data; name="upload"; filename="upload.txt"\r\n'
                 'Content-Type: text/plain\r\n\r\nfile-data\r\n--' + boundary + '--\r\n')
    # Successful real file operations are required observations. The model
    # creates no files and does not infer success from a recorded product run.
    values += [True, len('file-data'.encode()) > 0,
               'name="upload"; filename="upload.txt"' in file_body,
               'Content-Type: text/plain\r\n\r\n' in file_body,
               '\r\n\r\nfile-data\r\n' in file_body]
    size = len('abcd'.encode())
    accepted = size <= 3
    values += [accepted, size if accepted else 0, True, size, True,
               accepted, size if accepted else 0, True]
    return values


def expected_rows():
    if len(_INPUTS) != 11 or set(_PROGRAMS) != {'csv', 'multipart', 'slice_helper', 'atomic_fatal'}:
        raise ValueError('complete library model lost a reviewed input or program')
    for name, digest in _INPUTS.items():
        raw = (ROOT / name).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError('complete library source or declaration changed: ' + name)
    observations = {'csv': csv_observations(), 'multipart': multipart_observations(),
                    'slice_helper': [], 'atomic_fatal': ['-- before --']}
    scopes = {
        'csv': 'Full document, all six report observations, mismatch catch, semicolon/tab, quoting, comments and column limit. No diagnostic field is removed.',
        'multipart': 'Full field/file framing and limits, successful real file writes, and deferred directory deletion. Fault-path cleanup remains open.',
        'slice_helper': 'Standalone declaration-only source has no call or output. Exported Slice return, importer and borrow lifetime remain open.',
        'atomic_fatal': 'The first print completes, then division by zero in the final formatter discards the entire second print group. Uncaught panic exits one with the normative E0420 line; compilation refusal is not the required execution failure.'}
    rows = []
    for name, values in observations.items():
        original, selected = _PROGRAMS[name]
        render = lambda value: str(value).lower() if type(value) is bool else str(value)
        rows.append({'id': 'complete_library_' + name, 'source': selected,
                     'original_source': original, 'required_target': None,
                     'expected': {'exit_code': 1 if name == 'atomic_fatal' else 0,
                                  'stdout_utf8': ''.join(render(v) + '\n' for v in values),
                                  'stderr_utf8': '[Uncaught Panic] E0420: division by zero\n' if name == 'atomic_fatal' else ''},
                     'model': scopes[name], 'observation_count': len(values)})
    return rows
