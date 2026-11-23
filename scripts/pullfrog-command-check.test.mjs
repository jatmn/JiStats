import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import path from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

const owner = { id: 12479882, login: "jatmn" };
const repository = "jatmn/JiStats";

/** @param {{pullRequest: boolean, body?: string, attempt?: string}} options */
function runOwnerCommand({
  pullRequest,
  body = "@pullfrog summarize this thread",
  attempt = "1",
}) {
  const dir = mkdtempSync(path.join(tmpdir(), "jistats-owner-command-"));
  const eventPath = path.join(dir, "event.json");
  const outputPath = path.join(dir, "output");
  try {
    const issue = { number: 123 };
    if (pullRequest) {
      Object.assign(issue, { pull_request: {} });
    }
    writeFileSync(
      eventPath,
      JSON.stringify({
        action: "created",
        sender: owner,
        repository: { full_name: repository },
        issue,
        comment: { id: 456, user: owner, body },
      }),
    );
    writeFileSync(outputPath, "");
    const result = spawnSync(
      process.execPath,
      [fileURLToPath(new URL("pullfrog-command-check.mjs", import.meta.url))],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          GITHUB_ACTOR: owner.login,
          GITHUB_EVENT_NAME: "issue_comment",
          GITHUB_EVENT_PATH: eventPath,
          GITHUB_OUTPUT: outputPath,
          GITHUB_RUN_ATTEMPT: attempt,
          GITHUB_TRIGGERING_ACTOR: owner.login,
        },
        timeout: 5000,
      },
    );
    assert.ifError(result.error);
    assert.equal(result.status, 0, result.stderr);
    return { eventPath, output: readFileSync(outputPath, "utf8") };
  } finally {
    rmSync(dir, { force: true, recursive: true });
  }
}

/** @param {string} output */
function readOwnerCommandPayload(output) {
  const [payload, authorized, trailing] = output.split("\n");
  assert.ok(payload.startsWith("payload="));
  assert.equal(authorized, "authorized=true");
  assert.equal(trailing, "");
  assert.equal(output.split("\n").length, 3);
  /** @type {unknown} */
  const command = JSON.parse(payload.slice("payload=".length));
  return (
    /** @type {{prompt: string, event: {issue_number: number, comment_id: number, is_pr?: boolean}}} */
    (command)
  );
}

test("writes issue and PR comment commands to GitHub output", () => {
  const body = "@pullfrog summarize this thread\nInclude the second line";
  /** @type {Array<[boolean, string]>} */
  const cases = [
    [false, "issues"],
    [true, "pull"],
  ];
  for (const [pullRequest, threadPath] of cases) {
    const payload = readOwnerCommandPayload(
      runOwnerCommand({ pullRequest, body }).output,
    );
    assert.ok(
      payload.prompt.includes(
        `${repository}/${threadPath}/123#issuecomment-456`,
      ),
    );
    assert.ok(payload.prompt.includes(body));
    assert.equal(payload.event.issue_number, 123);
    assert.equal(payload.event.comment_id, 456);
    assert.equal(payload.event.is_pr, pullRequest || undefined);
  }
});

test("uses the event snapshot for oversized owner commands", () => {
  const body = `@pullfrog summarize ${"界".repeat(25000)}`;
  const { eventPath, output } = runOwnerCommand({ pullRequest: true, body });
  const payload = readOwnerCommandPayload(output);
  assert.ok(payload.prompt.includes(JSON.stringify(eventPath)));
  assert.ok(payload.prompt.includes("/pull/123#issuecomment-456"));
  assert.ok(!payload.prompt.includes(body));
});

test("keeps GitHub output empty for ineligible owner commands", () => {
  assert.equal(
    runOwnerCommand({
      pullRequest: false,
      body: "Mentioning @pullfrog in prose",
    }).output,
    "",
  );
  assert.equal(runOwnerCommand({ pullRequest: true, attempt: "2" }).output, "");
});
