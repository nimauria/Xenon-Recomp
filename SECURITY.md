# Security policy

Xenon is experimental software under active development. It has no public release, so only the current development branch (`development-restructure`) is in scope.

## Reporting a vulnerability

**Please do not report security vulnerabilities in public issues, pull requests or discussions.**

Report them privately through GitHub: open the repository's **Security** tab and choose **Report a vulnerability**. If that option is not available, open an issue that asks for a private contact, without including any details of the vulnerability.

Please include:

- the affected component, file or commit;
- what an attacker could do, and under what conditions;
- steps or a minimal proof of concept to reproduce it;
- any suggested fix.

Do not include proprietary Xbox 360 content, such as game executables, disc images, firmware or keys, in a report. A synthetic input that triggers the problem is preferred.

## What is in scope

Areas where Xenon handles untrusted input or touches the network are the most relevant:

- parsing of user-supplied content: XEX executables, GDFX disc images, STFS packages and module manifests;
- the Xenon Network client (`src/network/`) and its HTTPS transport;
- module and launcher package download, installation and update;
- the launcher/runtime-host process contract;
- privacy of diagnostics and support bundles.

Incorrect emulation of Xbox 360 behaviour inside the guest is a correctness bug, not a security vulnerability, unless it lets guest code or content affect the host outside Xenon's intended boundaries.

## What to expect

Xenon is maintained by a single developer, so responses are best effort and there is no guaranteed response time. The aim is to acknowledge valid reports, fix them in the development branch and credit the reporter if they wish.

Xenon's network and packaging code includes defensive controls, described in [`docs/network/XENON_NETWORK_V1.md`](docs/network/XENON_NETWORK_V1.md), but the project makes no claim of being secure, and it has not had an independent security review.
