# Contributing to JiStats

Open a pull request against the default branch and describe the behavior you changed, how you validated it, and any checks that still need to run. The repository owner handles merges. Follow [AGENTS.md](AGENTS.md) for the project conventions and [the test strategy](docs/test-strategy.md) for verification.

Keep provider captures, credentials, and personal data out of public issues and pull requests. Use synthetic or redacted [fixtures](docs/fixtures/README.md) in tests. Send vulnerability reports through the [security policy](SECURITY.md).

Dependabot checks GitHub Actions for version updates. Review bundled C++ libraries separately when upgrading dependencies. When updating Pullfrog, review the action pin and command payload version together.

### Pullfrog

In the Pullfrog console, keep automatic PR reviews, draft reviews, and re-reviews on new commits enabled, with reviews for external contributors disabled. The [workflow](.github/workflows/pullfrog.yml) checks the PR author against GitHub before starting a review. Automatic reviews cover PRs opened by `jatmn`, even when someone else pushes to them. The owner can still use `@pullfrog` commands in issue and PR comments.
