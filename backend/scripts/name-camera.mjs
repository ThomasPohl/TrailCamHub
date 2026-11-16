#!/usr/bin/env node
// Gives a camera the name the user sees in the gallery (#273), e.g. "Garten".
//
//   npm run name-camera -- --camera F4_C1_56_06_ED_3B --name "Garten" \
//     [--stack trailcamhub | --function <name>] [--region <region>]
//
// --camera is the camId (the camera's folder on the hub's SD card, also
// shown in the gallery as long as the camera has no name). Needs the AWS CLI
// like register-hub.

import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { parseArgs } from 'node:util';

const { values } = parseArgs({
  options: {
    camera: { type: 'string' },
    name: { type: 'string' },
    stack: { type: 'string', default: 'trailcamhub' },
    function: { type: 'string' },
    region: { type: 'string' },
  },
});

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(1);
}

if (!values.camera) fail('--camera is required');
if (!values.name) fail('--name is required');

function aws(args) {
  try {
    return execFileSync('aws', values.region ? [...args, '--region', values.region] : args, {
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

/** Runs an admin task in the function and returns its JSON result. */
function invoke(event) {
  // The response may hold the hub key, so it goes to a private temporary
  // file rather than a fixed path.
  const dir = mkdtempSync(join(tmpdir(), 'name-camera-'));
  const outFile = join(dir, 'response.json');
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
    const text = readFileSync(outFile, 'utf8');
    if (functionError && functionError !== 'None') {
      fail(`function ${functionName} failed: ${text}`);
    }
    return JSON.parse(text);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

const result = invoke({ admin: 'renameCamera', camId: values.camera, name: values.name });
if (!result.ok) fail(result.error);
console.log(`Camera ${result.camId} is now called "${result.name}".`);
