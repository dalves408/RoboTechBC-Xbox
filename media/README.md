# Robotech: Battlecry Recomp — release art

All artwork here is original: an original chevron mark in the F1 overlay palette (navy grid, pale blue, amber),
original type set in Inter / JetBrains Mono, and procedural skylines and contrails. Nothing is traced from or based on
the strategy guide's or the franchise's artwork, characters, mech designs or logos. Only the guide's text was used,
for mood (the "RDF field manual" military-briefing feel). Rebuild everything with
`python3 source/make_release_art.py <out_dir>` (needs Pillow + numpy, Inter and JetBrains Mono fonts).

RecompLauncher's docs and recomplauncher.json schema set no art sizes, so these follow common launcher sizes.

| Folder | Files | Notes |
|---|---|---|
| boxart/ | 600x900, 1200x1800, 2400x3600 PNG | 2:3 cover |
| logo/ | 1280, 1920 and ~4K wide PNG | transparent background |
| backdrop/ | 3840x2160 PNG + JPG, 1920x1080 JPG, 3840x1240 hero, 4K with logo | left side kept dark for UI/logo |
| icon/ | 256/512/1024 PNG + robotech.ico | game icon (dusk skyline); also `res/robotech.ico` |
| screenshots/ | 6 x 1920x1080 PNG | real gameplay frames from recordings of this build; 01-04 are native 1080p, 05-06 are from the 720p recording, upscaled |
