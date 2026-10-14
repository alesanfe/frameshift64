## Summary

<!-- What changes and why. Link the issue/bug doc. -->

## Testing performed

<!-- REQUIRED (project rule, OpenMW-style). What did you actually run?
     e.g. "built Debug+Release, localtest 60s IN SYNC, unit codec suite" -->

## Checklist

- [ ] Build passes (`cmake --build` clean of new warnings)
- [ ] Tests added/updated for the change (regression test for bugfixes)
- [ ] Docs updated if behavior/config/protocol changed (`check_docs` clean)
- [ ] If this touches `decomp/src/sys/netpeer*` or `src/netplay/`:
      `docs/netplay/protocol/` updated or justification given
- [ ] If this touches a public header in `include/smash_classic/`:
      `CHANGELOG.md` updated
- [ ] No ROM-derived or non-redistributable data included
