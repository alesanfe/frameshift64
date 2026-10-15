# src/core/config — settings schema

`schema.cpp` declares the CVar/settings surface the menu and CVars
system expose (defaults, ranges, categories). Validated by
`tools/analysis/lint/check_cvar_schema.py`.

## Allowed dependencies

LUS CVar API, `observability`. Must not import feature, netplay, or
presentation internals — it only describes settings.

## Tests

`check_cvar_schema.py` lint gate; menu wiring covered by e2e menu tests.
