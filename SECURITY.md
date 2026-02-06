# Security Policy

sendrawtx is a server that handles Bitcoin transactions. Security issues get priority treatment.

## Reporting a Vulnerability

**Do NOT open a public issue.**

Use [GitHub Security Advisories](https://github.com/8144225309/sendrawtx/security/advisories/new) to report vulnerabilities privately.

## What Counts as a Security Issue

- Memory safety bugs (buffer overflow, use-after-free, double-free)
- Injection (command injection, header injection, path traversal)
- Authentication or authorization bypass
- Denial of service (crash, resource exhaustion, slowloris bypass)
- Information disclosure (leaking memory contents, stack data)
- TLS/crypto weaknesses
- Anything that could compromise a Bitcoin node behind the relay

## What's NOT a Security Issue

- Feature requests
- Build failures on unsupported platforms
- Cosmetic bugs in HTML responses
- Performance issues (unless they enable DoS)

Open a regular issue for those.

## Response Timeline

- **48-72 hours**: Acknowledgment that we received your report
- **7 days**: Initial assessment and severity classification
- **30 days**: Fix developed and tested (for confirmed issues)

Critical issues (remote code execution, node compromise) get dropped-everything priority.

## Supported Versions

| Version | Supported |
|---------|-----------|
| main branch (latest) | Yes |
| Older commits | No |

We don't backport fixes. Update to latest.

## Disclosure

We'll coordinate disclosure timing with you. We won't publish details until a fix is available and deployed.

Credit is given to reporters unless they prefer to stay anonymous.
