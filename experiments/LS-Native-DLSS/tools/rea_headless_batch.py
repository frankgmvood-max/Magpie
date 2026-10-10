#!/usr/bin/env python3
"""Run a pinned REA read-only Ghidra bridge over private files, without a daemon.

The upstream MCP provider needs host PID inspection and a local socket. Managed
environments may not provide those. This adapter runs an ordinary bounded
headless child, retaining REA's version, input digest, request and analysis checks.
Results are bridge responses, NOT an upstream MCP evidence ledger.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess

BRIDGE_SHA256 = 'f846f430fb225f2dd839699a25b6b59cc5338733cf212e1ca5fb8e396eeba590'
READ_ONLY = {'ping', 'inspect_native_load_image', 'list_documents',
             'procedure_info', 'procedure_pseudo_code', 'procedure_callers',
             'procedure_callees', 'read_function_instructions', 'analyze_function',
             'inspect_native_instruction'}


def private_json(path, value):
    with path.open('x', encoding='utf-8') as stream:
        os.chmod(path, 0o600)
        json.dump(value, stream, indent=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--requests', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prefix = Path(os.environ['LS_REA_PREFIX']).resolve()
    ghidra = Path(os.environ['GHIDRA_INSTALL_DIR']).resolve()
    rea = prefix / 'node_modules/rea-agents'
    if json.loads((rea / 'package.json').read_text())['version'] != '6.3.0':
        raise ValueError('Only reviewed REA 6.3.0 is admitted.')
    upstream = rea / 'bridge/ghidra/ReaGhidraBridge.java'
    source_bytes = upstream.read_bytes()
    if hashlib.sha256(source_bytes).hexdigest() != BRIDGE_SHA256:
        raise ValueError('REA bridge source digest mismatch.')
    requests = json.loads(args.requests.read_text())
    if not isinstance(requests, list) or not 1 <= len(requests) <= 64:
        raise ValueError('Expected 1..64 bounded requests.')
    for request in requests:
        if set(request) != {'method', 'params'} or request['method'] not in READ_ONLY:
            raise ValueError('Only explicitly admitted read-only bridge requests are allowed.')
    output = args.output.resolve()
    output.mkdir(mode=0o700)  # Refuse reuse: no overwrites or stale results.
    os.chmod(output, 0o700)
    binary = output / args.binary.name
    shutil.copyfile(args.binary.resolve(), binary)
    os.chmod(binary, 0o400)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if digest != args.sha256.lower():
        raise ValueError('Immutable target snapshot digest mismatch.')
    scripts = output / 'scripts'
    scripts.mkdir(mode=0o700)
    for path in (rea / 'bridge/ghidra').glob('*.java'):
        shutil.copyfile(path, scripts / path.name)
    shutil.copyfile(rea / 'LICENSE', scripts / 'REA-LICENSE.txt')
    source = source_bytes.decode('utf-8')
    old = '            serve(descriptor);'
    if source.count(old) != 1:
        raise ValueError('Reviewed bridge patch location changed.')
    source = source.replace(old, '''            if (!descriptor.transport.equals("file-batch"))
                throw new IllegalArgumentException("Batch adapter requires file-batch");
            Path requests = Path.of(descriptor.endpointPath);
            try (BufferedReader reader = Files.newBufferedReader(requests, StandardCharsets.UTF_8);
                 BufferedWriter writer = Files.newBufferedWriter(
                     Path.of(descriptor.endpointPath + ".results"), StandardCharsets.UTF_8,
                     StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
                Files.setPosixFilePermissions(Path.of(descriptor.endpointPath + ".results"),
                    PosixFilePermissions.fromString("rw-------"));
                serveClient(descriptor, reader, writer);
            }''')
    (scripts / upstream.name).write_text(source, encoding='utf-8')
    token = secrets.token_hex(32)
    request_path = output / 'requests.jsonl'
    with request_path.open('x', encoding='utf-8') as stream:
        os.chmod(request_path, 0o600)
        for index, request in enumerate(requests, 1):
            stream.write(json.dumps({'id': index, 'token': token, **request}) + '\n')
    descriptor = output / 'descriptor.json'
    private_json(descriptor, {
        'transport': 'file-batch', 'endpoint_path': str(request_path), 'token': token,
        'run_id': output.name, 'target_sha256': digest,
        'provider_version': '12.1.4', 'profile_digest': 'ls-read-only-batch-v1',
    })
    project = output / 'project'
    project.mkdir(mode=0o700)
    command = [str(ghidra / 'support/analyzeHeadless'), str(project), 'LS',
               '-import', str(binary), '-scriptPath', str(scripts),
               '-postScript', upstream.name, str(descriptor),
               '-analysisTimeoutPerFile', '480', '-max-cpu', '2', '-deleteProject']
    try:
        with (output / 'ghidra.log').open('x', encoding='utf-8') as log:
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                       timeout=660, check=False)
    finally:
        # Also remove bearer input when startup or the bounded child fails.
        descriptor.unlink(missing_ok=True)
        request_path.unlink(missing_ok=True)
    result_path = Path(str(request_path) + '.results')
    if completed.returncode != 0 or not result_path.is_file():
        raise RuntimeError('Headless batch failed; inspect private ghidra.log.')
    responses = [json.loads(line) for line in result_path.read_text().splitlines()]
    if [row.get('id') for row in responses] != list(range(1, len(requests) + 1)):
        raise RuntimeError('Missing or reordered bridge responses.')
    private_json(output / 'provenance.json', {
        'adapter': 'rea-headless-file-batch-v1', 'rea': '6.3.0', 'ghidra': '12.1.4',
        'bridge_sha256': BRIDGE_SHA256, 'target_sha256': digest,
        'requests': requests, 'responses_sha256': hashlib.sha256(result_path.read_bytes()).hexdigest(),
        'limitations': ['Static observations only; no runtime GPU evidence.',
                        'Bridge responses, not the upstream MCP evidence ledger.'],
    })
    failures = sum('error' in row for row in responses)
    print(json.dumps({'responses': len(responses), 'errors': failures, 'output': str(output)}))
    if failures:
        raise RuntimeError('Bridge returned errors; inspect private results.')


if __name__ == '__main__':
    main()
