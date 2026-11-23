import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { test } from 'node:test';
import timers from 'node:timers/promises';
import { isAuthorizedReview } from './pullfrog-review-check.mjs';

const owner = { id: 12479882, login: 'jatmn' };
const api = {
  apiUrl: 'https://api.github.test',
  repository: 'jatmn/JiStats',
  token: 'fixture-token',
};

function review(trigger = 'pull_request_opened', fields = {}) {
  return {
    '~pullfrog': true,
    triggerer: 'another-pusher',
    event: { trigger, is_pr: true, issue_number: 7, ...fields },
  };
}

test('accepts opening a PR, marking it ready, and pushing to it, including drafts', async (t) => {
  const cases = [
    ['pull_request_opened', false],
    ['pull_request_opened', true],
    ['pull_request_ready_for_review', false],
    ['pull_request_synchronize', false],
    ['pull_request_synchronize', true],
  ];
  for (const [trigger, draft] of cases) {
    const fetch = t.mock.method(globalThis, 'fetch', async (url, { headers, signal }) => {
      assert.equal(url, 'https://api.github.test/repos/jatmn/JiStats/pulls/7');
      assert.equal(headers.Authorization, 'Bearer fixture-token');
      assert.ok(signal instanceof AbortSignal);
      return Response.json({ user: owner, draft });
    });
    try {
      assert.equal(await isAuthorizedReview(review(trigger), api), true, trigger);
      assert.equal(fetch.mock.callCount(), 1);
    } finally {
      fetch.mock.restore();
    }
  }
});

test('rejects invalid and unrelated dispatches before calling GitHub', async (t) => {
  const fetch = t.mock.method(globalThis, 'fetch');
  for (const payload of [
    null, {}, { event: review().event },
    review('issue_comment_created'),
    review(undefined, { is_pr: false }),
    review(undefined, { issue_number: '7' }),
    review(undefined, { issue_number: 0 }),
  ]) {
    assert.equal(await isAuthorizedReview(payload, api), false);
  }
  assert.equal(fetch.mock.callCount(), 0);
});

test('requires both author identifiers, even when the owner pushes', async (t) => {
  const authors = [null, { ...owner, id: 1 }, { ...owner, login: 'another-author' }];
  t.mock.method(globalThis, 'fetch', async () => Response.json({ user: authors.shift() }));
  const payload = { ...review('pull_request_synchronize'), triggerer: owner.login };
  for (let i = 0; i < 3; i++) {
    assert.equal(await isAuthorizedReview(payload, api), false);
  }
});

test('retries a timed-out response body and an upstream failure', async (t) => {
  const delay = t.mock.method(timers, 'setTimeout', async () => {});
  const timeout = t.mock.method(AbortSignal, 'timeout', () => AbortSignal.abort(new DOMException('timed out', 'TimeoutError')));
  const responses = [
    async (signal) => ({ ok: true, json: async () => signal.throwIfAborted() }),
    async () => new Response(null, { status: 503 }),
    async () => Response.json({ user: owner }),
  ];
  const fetch = t.mock.method(globalThis, 'fetch', async (_url, { signal }) => responses.shift()(signal));
  assert.equal(await isAuthorizedReview(review(), api), true);
  assert.equal(fetch.mock.callCount(), 3);
  assert.deepEqual(timeout.mock.calls.map((call) => call.arguments), [[10_000], [10_000], [10_000]]);
  assert.deepEqual(delay.mock.calls.map((call) => call.arguments), [[250], [750]]);
});

test('fails after three transient errors', async (t) => {
  const delay = t.mock.method(timers, 'setTimeout', async () => {});
  const fetch = t.mock.method(globalThis, 'fetch', async () => {
    throw new TypeError('fetch failed');
  });
  await assert.rejects(isAuthorizedReview(review(), api), /Could not verify PR #7 author/);
  assert.equal(fetch.mock.callCount(), 3);
  assert.deepEqual(delay.mock.calls.map((call) => call.arguments), [[250], [750]]);
});

test('fails immediately for access errors and rate limits', async (t) => {
  const errors = [
    { status: 403 },
    { status: 429 },
    { status: 503, headers: { 'Retry-After': '60' } },
  ];
  const fetch = t.mock.method(globalThis, 'fetch', async () => new Response(null, errors.shift()));
  for (const [index, status] of [403, 429, 503].entries()) {
    await assert.rejects(isAuthorizedReview(review(), api), (error) => {
      assert.equal(error.cause.message, `GitHub HTTP ${status}`);
      return true;
    });
    assert.equal(fetch.mock.callCount(), index + 1);
  }
});

test('requires verified author on the automatic review step', () => {
  const workflow = readFileSync(new URL('../.github/workflows/pullfrog.yml', import.meta.url), 'utf8');
  const [, automatic] = workflow.split('\n  auto-review:\n');
  assert.ok(automatic, 'Expected the automatic review job');
  const job = automatic.split(/^ {2}\S/m, 1)[0];
  const condition = job.match(/^ {4}if: >-\n((?: {6}\S[^\n]*\n)+)/m)?.[1];
  assert.ok(condition?.split('\n').includes("      github.repository == 'jatmn/JiStats' &&"));
  const steps = job.split(/^ {6}- /m);
  const authorize = steps.find((step) => step.startsWith('name: Verify the pull request author\n'));
  const review = steps.find((step) => step.startsWith('name: Review the pull request\n'));
  assert.ok(authorize && review, 'Expected authorization and review steps');
  assert.ok(steps.indexOf(authorize) < steps.indexOf(review));
  assert.ok(authorize.split('\n').includes('        id: authorize'));
  assert.ok(authorize.split('\n').includes('        run: node scripts/pullfrog-review-check.mjs'));
  assert.ok(review.split('\n').includes("        if: steps.authorize.outputs.authorized == 'true'"));
  assert.match(review, /^ {8}uses: pullfrog\/pullfrog@[0-9a-f]{40}(?: +#.*)?$/m);
  const [, withInputs] = review.split('\n        with:\n');
  const inputs = withInputs?.split(/^ {8}\S/m, 1)[0];
  assert.ok(inputs?.split('\n').includes('          prompt: ${{ inputs.prompt }}'));
});
