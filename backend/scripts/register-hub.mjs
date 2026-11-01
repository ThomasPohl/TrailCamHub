#!/usr/bin/env node
// Registers a hub, links Telegram users to it, and prints its key.
//
//   npm run register-hub -- --hub garden-hub [--name "Garden"] \
//     [--user <telegramUserId>]... [--rotate-key] \
//     [--stack trailcamhub | --function <name>] [--region eu-central-1]
//
// - A new hub gets a key, printed once.
// - For an existing hub, --user links more people (family members) without
//   touching the key; --rotate-key issues a new key (the old one stops working).
// - Cameras are not registered by hand: they appear automatically when the
//   hub reports them, and every user linked to the hub sees them.
//
// The script invokes the backend's Lambda function through the AWS CLI, and
// the function writes to the table. AWS projects deny people direct access to
// DynamoDB data, but the function may still use its table. Needs the AWS CLI
// with credentials allowed lambda:InvokeFunction (and, to find the function,
// cloudformation:DescribeStackResources).

import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { parseArgs } from 'node:util';

const { values } = parseArgs({
  options: {
    hub: { type: 'string' },
    name: { type: 'string' },
    user: { type: 'string', multiple: true, default: [] },
    'rotate-key': { type: 'boolean', default: false },
    stack: { type: 'string', default: 'trailcamhub' },
    function: { type: 'string' },
    table: { type: 'string' }, // no longer needed; accepted for old command lines
    region: { type: 'string', default: process.env.AWS_REGION ?? 'eu-central-1' },
  },
});

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(1);
}

if (!values.hub || !/^[A-Za-z0-9_-]{1,64}$/.test(values.hub)) {
  fail('--hub must be 1-64 characters [A-Za-z0-9_-]');
}
for (const user of values.user) {
  if (!/^\d+$/.test(user)) fail(`--user must be a numeric Telegram user id (got ${user})`);
}
if (values.table) console.warn('note: --table is no longer needed and is ignored.');

function aws(args) {
  try {
    return execFileSync('aws', [...args, '--region', values.region], {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'inherit'],
    }).trim();
  } catch (error) {
    if (error.code === 'ENOENT') fail('the AWS CLI (aws) is not installed or not in PATH');
    fail(`aws ${args.slice(0, 2).join(' ')} failed`);
  }
}

function findFunction() {
  const name = aws([
    'cloudformation',
    'describe-stack-resources',
    '--stack-name',
    values.stack,
    '--query',
    "StackResources[?ResourceType=='AWS::Lambda::Function'].PhysicalResourceId | [0]",
    '--output',
    'text',
  ]);
  if (!name || name === 'None') fail(`no Lambda function in stack ${values.stack}`);
  return name;
}

const functionName = values.function ?? findFunction();
const event = {
  admin: 'registerHub',
  hubId: values.hub,
  name: values.name,
  users: values.user.map(Number),
  rotateKey: values['rotate-key'],
};

// The response holds the hub key, so it goes to a private temporary file
// rather than a fixed path.
const dir = mkdtempSync(join(tmpdir(), 'register-hub-'));
const outFile = join(dir, 'response.json');
let text;
try {
  const functionError = aws([
    'lambda',
    'invoke',
    '--function-name',
    functionName,
    '--cli-binary-format',
    'raw-in-base64-out',
    '--payload',
    JSON.stringify(event),
    '--query',
    'FunctionError',
    '--output',
    'text',
    outFile,
  ]);
  text = readFileSync(outFile, 'utf8');
  if (functionError && functionError !== 'None') {
    fail(`function ${functionName} failed: ${text}`);
  }
} finally {
  rmSync(dir, { recursive: true, force: true });
}

const result = JSON.parse(text);
if (!result.ok) fail(result.error);

for (const user of result.linkedUsers) {
  console.log(`Linked Telegram user ${user} to hub ${values.hub}.`);
}
if (result.created) {
  console.log(`Hub ${values.hub} registered.`);
} else if (result.key) {
  console.log(`Key of hub ${values.hub} rotated; the old key no longer works.`);
}
if (result.key) {
  console.log('\nHub key (enter it as "Device key" in the web installer; shown only once):\n');
  console.log(`  ${result.key}\n`);
} else if (result.linkedUsers.length === 0 && !values.name) {
  console.log('Nothing to do: the hub exists. Use --user, --name or --rotate-key.');
}
