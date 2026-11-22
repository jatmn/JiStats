import assert from 'node:assert/strict';
import { test } from 'node:test';
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
});

test('fails after three transient errors', async (t) => {
  const fetch = t.mock.method(globalThis, 'fetch', async () => {
    throw new TypeError('fetch failed');
  });
  await assert.rejects(isAuthorizedReview(review(), api), /Could not verify PR #7 author/);
  assert.equal(fetch.mock.callCount(), 3);
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
