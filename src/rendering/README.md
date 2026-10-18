# src/rendering — display-list range validation

`port_dl_ranges` bounds-checks display-list reads against known RDRAM
segments — the guard that turns wild pointers into diagnosable faults
instead of silent corruption (see `docs/bugs/`).

## Allowed dependencies

decomp segment headers, `observability`. No UI, no features.

## Tests

Exercised by every headless/e2e run; targeted checks belong in
`tests/unit/rendering/` (planned).
