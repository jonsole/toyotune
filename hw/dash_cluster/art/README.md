# Artwork for the dash nodes' power-on splash

| File | What | Used for |
|---|---|---|
| `toyota_emblem.png` | The emblem on a transparent background, 1465x1000, cut from `toyota_logo_hd.jpg` - edit this one by hand if it needs it | used as it is, in preference to cutting one out of the photographs |
| `toyota_logo_hd.jpg` | Satin emblem on white, 1920x1080 | the source of `toyota_emblem.png` - its metal is never as white as the background, so it cuts cleanly |
| `toyota_emblem_cut.png` | The emblem as cut automatically, transparent background, 609x384 | a starting point for editing - regenerated each run, not used |
| `toyota_logo_dark.png` | Toyota emblem, chrome, on carbon-fibre weave, 706x452 | fades in on every node at power-on |
| `toyota_logo.jpg` | The same emblem on white, 370x300 | not used - its highlights are the background's exact white where they meet the rings, so it cannot be cut out |
| `mr2_logo.png` | The MR2 badge, white letters on black, 786x175 | one letter per node after the emblem: M, R, 2 |
| `mr2_badge.jpg` | The same badge in red, 546x366 | not used - its letters are 70 px tall, and scaled up they rippled |

Supplied by Jon on 2026-09-19 for his own car's dash. All of them are Toyota trademarks
in someone else's photographs, and this repository is public - so they are not
committed without a decision to do so.

The firmware never reads these files. `tools/splash/` cuts the emblem out of its
white background and each letter out of the badge, scales them to the 466x466
panel and writes paletted images into the firmware - run it again after
changing anything here.
