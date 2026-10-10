#!/usr/bin/env node
// One persistent REA MCP session for a bounded, analyst-directed investigation.
// The runtime and providers are caller-installed; no agent configuration changes.
import { createRequire } from 'node:module';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { createInterface } from 'node:readline';

const prefix = process.env.LS_REA_PREFIX;
if (!prefix || !process.env.GHIDRA_INSTALL_DIR || !process.env.JAVA_HOME) {
  throw new Error('Set LS_REA_PREFIX, GHIDRA_INSTALL_DIR and JAVA_HOME to the verified local installations.');
}
const output = resolve(process.argv[2] ?? 'local/rea');
await mkdir(output, { recursive: true, mode: 0o700 });
const require = createRequire(join(resolve(prefix), 'package.json'));
const { Client } = require('@modelcontextprotocol/client');
const { StdioClientTransport, getDefaultEnvironment } = require('@modelcontextprotocol/client/stdio');
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [join(resolve(prefix), 'node_modules/rea-agents/scripts/rea.mjs'), '--mcp'],
  env: {
    ...getDefaultEnvironment(),
    GHIDRA_INSTALL_DIR: process.env.GHIDRA_INSTALL_DIR,
    JAVA_HOME: process.env.JAVA_HOME,
    REA_GHIDRA_STARTUP_TIMEOUT_MS: process.env.REA_GHIDRA_STARTUP_TIMEOUT_MS ?? '600000',
  },
  stderr: 'pipe',
  maxBufferSize: 64 * 1024 * 1024,
});
transport.stderr?.on('data', data => process.stderr.write(data));
const client = new Client({ name: 'ls-native-research', version: '1.0.0' }, { capabilities: {} });
let open = false;
let sequence = 0;
try {
  await client.connect(transport);
  const catalog = await client.listTools();
  await writeFile(join(output, 'tools.json'), JSON.stringify(catalog, null, 2), { mode: 0o600 });
  const names = new Set(catalog.tools.map(tool => tool.name));
  console.log(JSON.stringify({ ready: true, tools: names.size, catalog: join(output, 'tools.json') }));
  const input = createInterface({ input: process.stdin, crlfDelay: Infinity });
  for await (const line of input) {
    if (!line.trim()) continue;
    let request;
    try {
      request = JSON.parse(line);
      if (request.command === 'exit') break;
      if (!names.has(request.name)) throw new Error('Tool is absent from the connected REA catalog.');
      const label = request.label ?? `${++sequence}-${request.name}`;
      if (!/^[a-zA-Z0-9_-]+$/.test(label)) throw new Error('Use a plain filename label.');
      const result = await client.callTool({ name: request.name, arguments: request.arguments ?? {} }, { timeout: 660000 });
      const file = join(output, `${label}.json`);
      await writeFile(file, JSON.stringify(result, null, 2), { mode: 0o600 });
      if (request.name === 'open_binary' && !result.isError) open = true;
      if (request.name === 'close_binary' && !result.isError) open = false;
      console.log(JSON.stringify({ file, isError: Boolean(result.isError), structuredKeys: Object.keys(result.structuredContent ?? {}) }));
    } catch (error) {
      console.log(JSON.stringify({ error: String(error), request: request?.name ?? null }));
    }
  }
} finally {
  if (open) {
    try {
      const result = await client.callTool({ name: 'close_binary', arguments: {} }, { timeout: 30000 });
      await writeFile(join(output, 'close.json'), JSON.stringify(result, null, 2), { mode: 0o600 });
    } catch (error) {
      console.error(`REA close failed: ${error}`);
    }
  }
  await client.close();
}
