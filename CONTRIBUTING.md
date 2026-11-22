# Contributing to JiStats

Open a pull request against the default branch and describe the behavior you changed, how you validated it, and any checks that still need to run. The repository owner handles merges. Follow [AGENTS.md](AGENTS.md) for the project conventions and [the test strategy](docs/test-strategy.md) for verification.

Keep provider captures, credentials, and personal data out of public issues and pull requests. Use synthetic or redacted [fixtures](docs/fixtures/README.md) in tests. Send vulnerability reports through the [security policy](SECURITY.md).

Dependabot checks GitHub Actions for version updates. Review bundled C++ libraries separately when upgrading dependencies. When updating Pullfrog, review the action pin and command payload version together.
