#!/usr/bin/env node
// Creates a user of the gallery web app (#259), links hubs to the user, and
// prints the set-up link with the user's token.
//
//   npm run add-user -- --name "Opa" --hub garden-hub [--hub ...] [--admin] \
//     [--app-url https://thomaspohl.github.io/TrailCamHub/app/gallery.html] \
//     [--stack trailcamhub | --function <name>] [--region <region>]
//   npm run add-user -- --user <userId> [--hub ...]   # new token, more hubs
//   npm run add-user -- --user <userId> --revoke      # lock out (lost laptop)
//   npm run add-user -- --user <adminId> --rotate-totp # new TOTP secret (lost phone)
//
// An admin has a second factor (TOTP): a new admin gets a secret, shown once
// as an otpauth:// URI (add it to an authenticator app, e.g. by QR code). The
// gallery asks for the 6-digit code before it opens the admin view; the plain
// link of an admin only shows his own hubs. Admin accounts are changed only
// here; the admin view rotates and revokes ordinary users.
//
// Open the link once on the user's laptop: the gallery keeps the token, and
// the user never signs in. The token is in the link's #fragment, which the
// browser does not send to any server. Anyone with the link sees the user's
// cameras, so send it privately; a new token (--user) makes the old link
// useless. --revoke makes the token useless at once without a new one; a
// later --user <userId> gives the user a new link. Needs the AWS CLI like
// register-hub.

import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { parseArgs } from 'node:util';

const { values } = parseArgs({
  options: {
    name: { type: 'string' },
    user: { type: 'string' },
    hub: { type: 'string', multiple: true, default: [] },
    admin: { type: 'boolean', default: false },
    revoke: { type: 'boolean', default: false },
    'rotate-totp': { type: 'boolean', default: false },
    'app-url': {
      type: 'string',
      default: 'https://thomaspohl.github.io/TrailCamHub/app/gallery.html',
    },
    stack: { type: 'string', default: 'trailcamhub' },
    function: { type: 'string' },
    region: { type: 'string' },
  },
});

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(1);
}

if (!values.user && !values.name) fail('--name is required for a new user');
if (values.user && !/^\d{16}$/.test(values.user)) fail('--user must be a gallery user id');
if (values.revoke && !values.user) fail('--revoke needs --user <userId>');
if (values['rotate-totp'] && !values.user) fail('--rotate-totp needs --user <adminId>');
for (const hub of values.hub) {
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(hub)) fail(`--hub must be a hub id (got ${hub})`);
}

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
  const dir = mkdtempSync(join(tmpdir(), 'add-user-'));
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

if (values.revoke) {
  const revoked = invoke({ admin: 'revokeAppUser', userId: Number(values.user) });
  if (!revoked.ok) fail(revoked.error);
  console.log(`Locked out user ${revoked.userId} (${revoked.name}): the old link no longer works.`);
  process.exit(0);
}

const result = invoke({
  admin: 'createAppUser',
  name: values.name,
  userId: values.user === undefined ? undefined : Number(values.user),
  hubIds: values.hub,
  role: values.admin ? 'admin' : undefined,
  rotateTotp: values['rotate-totp'] || undefined,
});
if (!result.ok) fail(result.error);

for (const hub of result.linkedHubs) console.log(`Linked hub ${hub} to user ${result.userId}.`);
console.log(
  `\nSet-up link for user ${result.userId} (open it once on the laptop; shown only once):\n`,
);
console.log(`  ${values['app-url']}#token=${result.token}\n`);

if (result.totpSecret) {
  console.log(
    'Second factor (TOTP) for this admin, shown only once. Add it to an authenticator app:',
  );
  console.log(`\n  ${result.totpUri}\n`);
  console.log(`  or enter the secret by hand: ${result.totpSecret}\n`);
}
