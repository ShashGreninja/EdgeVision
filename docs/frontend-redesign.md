# EdgeVision frontend redesign

## Direction and user flow

Landing direction: **The product performs**, seed **1563894589**. App direction: **Data terminal**, seed **1075945128**. Both use locally bundled Switzer, green-black, bone and jade. The proof section was redrawn once to plain columns because the project has measured results rather than customer logos.

The central idea is to show a real evaluation visit becoming a small cloud event. The landing page introduces that idea, lets visitors select four processing stages, states measured results with their limits, and answers setup questions. The demo opens in Recorded visit mode so someone can understand the project without installing anything. Live simulator mode adds connection checks and a motion trigger. Events and Engineering provide progressively deeper detail. The demo script opens live mode directly.

Recorded playback uses captured firmware telemetry and verified AWS responses. It cannot issue live commands. Navigation preserves the chosen API, module and mode without propagating unrelated URL parameters. Live confirmation requires a new detection for the current device; old history and self-tests cannot complete a triggered visit.

## Before and after

The old dashboard opened with five empty metrics and an empty chart. The primary action appeared below those panels on mobile. The redesign puts a visit and its primary action first, then shows the camera-to-cloud journey and latest result. Diagnostics are available when requested.

| Check | Before | Final landing | Final app |
| --- | --- | --- | --- |
| Inspector failures | 0 | 0 | 0 |
| Inspector warnings | 3 | 3 | 1 |
| Main warning areas | Contrast, borders, blank panels up to 243 px | Native scrollbar alignment, internal text anchors, mobile repeated rows | 47 px result-well gap |
| Source lint | Baseline inspected | 0 failures, 0 warnings | 0 failures, 2 semantic warnings |

Inspected at 1440, 768 and 390 px; opened all final fold screenshots and page contact sheets. The landing headline stays on two lines. The guide has no horizontal overflow at 390 px. The app lint warnings concern zero-padded clocks and em dashes for unavailable data. These are intentional uses.

The landing alignment warning measures a 16 px native Windows scrollbar difference; the content frame is centered within the available viewport. The four stage fields use their own text anchors inside a shared outer frame. Repeated mobile rows are the sequential pipeline and FAQ disclosures. The app reserves space for an upcoming event to reduce movement during playback. These warnings remain visible in the report rather than being suppressed.

## Critique

Self-assessment: design 7.8/10, usability 8.4/10, creativity 7.2/10, content 8.5/10; average 8.0/10. This is an implementation critique, not an independent user study.

The strongest improvement is the immediate recorded visit and the explicit distinction between recorded and live controls. The real footage gives the product a concrete subject. The restrained palette and consistent typography remove much of the previous dashboard clutter. During critique, the tablet headline was reduced to two lines, a redundant walkthrough was removed, and the processing strip itself became interactive. Stale telemetry now says Last received, and live setup collapses after a healthy connection unless explicitly opened.

The four-stage mobile walkthrough is still long, and the engineering view remains dense by necessity. More varied footage and observation of first-time users would be needed to judge whether the presentation works beyond this evaluation scene. Shutter and entrance animations include a reduced-motion fallback; that fallback was reviewed in source, not tested through runtime media emulation.

## Verification

- Ten focused Node tests pass for routing, event filtering, fresh-event confirmation, device isolation, self-test separation and payload calculations.
- JavaScript syntax and PowerShell parser checks pass. Git diff whitespace checks pass.
- Browser playback, pause/resume, reset, event filtering, JSON inspection, keyboard tabs and chart/table switching were exercised.
- In live mode, a browser motion trigger reached the firmware, new detections reached AWS, and the page displayed cloud confirmation. A self-test event was also stored. Fault injection was accepted, but a recovery-counter change was not established during that run.
- Disconnected state disables motion commands. Recorded mode disables simulator and fault commands.
- The five-page Word guide was exported through Microsoft Word, rendered to images, and visually checked page by page. Web, Word and PDF versions are included.

The frontend is a local preview; this change does not publish a frontend hosting service. Existing AWS backend resources remain in use. A physical Ring camera and Ring live-video input remain unverified; Playground events can wake the local simulator, whose detector reads the local evaluation clip.

## Assets and credits

Video and four stills come from the repository's existing `eval/clips/testclip.mp4`. They are labeled recorded evaluation footage. The repository does not identify the original creator or license, so confirm publication rights before public distribution. No customer or partner logos are implied.

Switzer fonts were obtained from the official Fontshare CDN. Source and license references, footage provenance and recording notes are in `dashboard/assets/credits.txt`. The recorded data contains simulator telemetry and event responses, without credentials. No new frontend framework or runtime dependency was added.
