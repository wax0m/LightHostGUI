# Security policy

## Supported versions

Fixes land on the `GUI` branch and in the next release. Older releases are not patched.

## Reporting a vulnerability

Please **do not open a public issue** for a security problem. Report it privately through GitHub:
**Security → Report a vulnerability** on this repository. Include the steps to reproduce, the affected
version or commit, and the impact you expect. You will get an answer as soon as possible, and the fix is
credited to you unless you prefer otherwise.

## Scope

LightHost GUI is a local desktop audio host. It opens no network ports, but it loads third-party plugin
binaries and reads its own settings and preset files. Relevant reports include:

- crashes, memory corruption or code execution triggered by a crafted settings or preset file;
- unsafe file handling (paths, permissions, temporary files);
- problems in the build chain, such as the pinned JUCE checkout.

Out of scope: vulnerabilities inside third-party plugins you choose to load. A hosted plugin runs inside
the app with the same privileges as the app itself, so only load plugins you trust.
