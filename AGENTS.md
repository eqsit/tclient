# Local boost development

The user asked to publish only the preserved kog-kinetix-8 baseline for Windows
and Linux. It is available at tag `v-tclient-plus-kog-8-20261004` in
`eqsit/tclient`; its source tree matches original commit `27df13e38`.

New boost changes are local work. Do not push these changes, create a public
branch or release for them, or replace the preserved release assets unless the
user explicitly asks. Implement, build, test and verify locally.

The user explicitly requested that the other avoid logic stay unchanged.
Keep movement, hook selection/pulses, waiting, input feedback and weapon return
from `27df13e38`. Boost only improves the aim of an already verified ordinary
rocket save; never add autonomous shots on safe ground.

Boost mode also preserves the rescue angle on its even fire-release counter
until server acknowledgement: a delayed press must not fire with mouse aim.
Keep fire counters, manual shots and hook aim unchanged. This is scoped to
boost shot delivery; do not change the baseline avoid/feedback algorithms.
When the F12 overlay is open, boost mode sends that computed shot/release
packet too. Keep this delivery correction disabled when boost is off.
