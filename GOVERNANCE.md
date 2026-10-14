# Governance

## Roles
- **Maintainer**: repository owner; final say on merges, releases, protocol
  and compatibility decisions.
- **Contributor**: anyone filing issues or PRs.

## Decisions
- Code merges require a green PR + maintainer review.
- Architectural changes land via an ADR in `docs/decisions/` (see existing
  ADRs for format); the ADR PR is the decision record.
- Compatibility-surface changes (protocol, save/config formats, mod API,
  lobby API) require the version bump rules in `docs/compatibility/policy.md`.

## Releases
Cut by the maintainer via `release.yml` on tags; artifacts are attested.
EOL/deprecation policy: `docs/engineering/deprecated-components.md`.
