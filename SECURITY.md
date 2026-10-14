# Security

The netplay lobby (`services/netplay-lobby/`) and the client's `src/netplay/` layer handle untrusted network input — protocol validation, `upkey` auth, rate limiting and star routing are the trust boundary.

- Report vulnerabilities privately to the maintainer (JRickey) rather than via public issues.
- Never commit ROMs, keys, or credentials. `upkey` values are per-session secrets, not repo material.
- Security-relevant docs: `docs/security/`, `docs/netplay/`, audits under `docs/investigations/netplay/`.
