# Attribution and scope

The presentation in this directory derives from OpenGhost by Andrew,
Copyright (c) 2026 Andrew, at OpenGhost 1.1.0 (tag `v1.1.0`, commit
`18a5a4a4a655bce2268455f9af90afe67406b507`), using its `linux/` directory.
The client was first derived from commit
`e1d8ca263d523466a787d0094e9390131c5b91c0` (1.0.1) and merged forward.
This is an independently adapted Ghosty client, **not official OpenGhost**.

The complete upstream terms are retained in [LICENSE](LICENSE). The name,
ghost artwork, animations and visual design are excluded from the upstream
MIT grant and have the stated non-commercial restrictions. Do not redistribute
this client as an unrestricted MIT-licensed design. The task owner confirmed
the requested use is permitted; that confirmation does not change the license.

`UPSTREAM.json` pins verbatim files and exact presentation fragments extracted
from `chat.js`. Its adapted entries identify the intentionally changed UI
entry points. Ghosty-specific transport, projection, settings and run-observation
code replaces OpenGhost's agent, provider, tools and session persistence; chat
passwords and encryption are not included.

The visible name changes to `ghosty`. Source-measured splash geometry and the
per-letter timing algorithm are unchanged; the shorter name naturally changes
its width and stagger-dependent total wait. Other changes are documented in
[the component manual](../docs/components/openghost-client.md), not treated as
proof of complete visual/behavioral parity.
