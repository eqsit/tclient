# Local boost development

The user asked to publish only the preserved kog-kinetix-8 baseline for Windows
and Linux. It is available at tag `v-tclient-plus-kog-8-20261004` in
`eqsit/tclient`; its source tree matches original commit `27df13e38`.

The user later explicitly requested publishing the checked kog-kinetix-10
version (`bf4bcf7dc`, public snapshot `d1d51a619`, tag
`v-tclient-plus-kog-10-20261004`). That publication is authorized.
Subsequent boost-strength/braking fixes remain local until separately requested.
Do not replace either preserved release. Implement, build and verify locally.

The user requested that other avoid logic stay unchanged, then specifically
authorized fixing unnecessary stops/interferences identified in logs and code.
Reproduce such a problem before changing it; do not broaden avoid changes.
Keep the baseline search, waiting, input feedback and weapon return from
`27df13e38`. Boost improves the aim of an already verified ordinary rocket
save; never add autonomous shots on safe ground.

The logged emergency-hook case at (2329,505), velocity (19.91,8.84), proves
that forced left movement can be avoided while keeping the same hook aim.
The user subsequently asked to prefer safe hook attachments that preserve
momentum, with backward pulls reserved for necessary rescues. Native replay
of the logged (8812,1742), (6680,2502) and (8480,2184) cases confirmed safe,
faster forward attachments. Boost may compare hook angles and pulses only
after a baseline rescue is verified, keeping that rescue as fallback. Check
the current rescue, no earlier next danger and no worse one-tick delay.
Retain the exact baseline selection with boost disabled.

The user also requested optional automatic resumption of an interrupted
manual hook. `tc_avoid_auto_rehook` defaults to 1; when disabled, suppress
the held manual hook after an avoid interruption until release/new press.
Keep emergency hooks available, track suppression separately per tee and
preserve the old input behavior when this option is enabled.

Boost mode also preserves the rescue angle on its even fire-release counter
until server acknowledgement: a delayed press must not fire with mouse aim.
Keep fire counters, manual shots and hook aim unchanged. This is scoped to
boost shot delivery; do not change the baseline avoid/feedback algorithms.
When the F12 overlay is open, boost mode sends that computed shot/release
packet too. Keep this delivery correction disabled when boost is off.
