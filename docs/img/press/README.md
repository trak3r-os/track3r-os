# Release promo images

Images posted with each release (Reddit, Discord, the release page). New Community OS material: drawn from the
firmware's own bitmap fonts, its Amiga theme palette and simulator screenshots; no Polyend artwork.

| File | Release |
|---|---|
| `track3r-os-v1.1.0-whats-new.png` | v1.1.0 |
| `track3r-os-v1.2.0-whats-new.png` | v1.2.0 |
| `track3r-os-v1.3.0-poster.png` | v1.3.0, features since v1.0.0 |
| `track3r-os-v1.4.0-cover.png` | v1.4.0, a 1990s computer-magazine cover (style homage only; no real magazine's name or logo) |

`make_poster.py` draws the v1.3.0 poster: `python3 -I docs/img/press/make_poster.py "$PWD" SHOT1 SHOT2 SHOT3 SHOT4`
(four 800x480 simulator screenshots); it writes the PNG next to itself.

`make_cover.py` draws the v1.4.0 cover (1200x2070), which lists everything new since v1.0 by issue: `python3 -I docs/img/press/make_cover.py "$PWD" MIXER_EQ`
(one 800x480 simulator screenshot of the MST EQ page; the scenario lines that make it are in the script's header); it writes the PNG next to itself.
