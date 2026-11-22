import { appendFileSync } from 'node:fs';
import { OWNER_ID } from './pullfrog-command-check.mjs';

const REVIEW_TRIGGERS = new Set([
  'pull_request_opened',
  'pull_request_ready_for_review',
  'pull_request_synchronize',
]);

const payload = JSON.parse(process.env.PULLFROG_PROMPT);
const event = payload?.event;
if (payload?.['~pullfrog'] !== true || event?.is_pr !== true ||
    !REVIEW_TRIGGERS.has(event.trigger) ||
    !Number.isSafeInteger(event.issue_number) || event.issue_number <= 0) {
  console.log('Skipping dispatch outside automatic PR review.');
  process.exit(0);
}

// The bot dispatches the workflow. GitHub's PR record identifies the author
// even when a different account pushed the commit.
const response = await fetch(
  `${process.env.GITHUB_API_URL}/repos/${process.env.GITHUB_REPOSITORY}/pulls/${event.issue_number}`,
  { headers: { Authorization: `Bearer ${process.env.GH_TOKEN}`, Accept: 'application/vnd.github+json' } },
);
if (!response.ok) {
  throw new Error(`Could not verify PR #${event.issue_number} author (GitHub HTTP ${response.status}).`);
}
const pr = await response.json();
if (pr.user?.id === OWNER_ID && pr.user.login === 'jatmn') {
  appendFileSync(process.env.GITHUB_OUTPUT, 'authorized=true\n');
} else {
  console.log('Skipping automatic review for a PR by another author.');
}
