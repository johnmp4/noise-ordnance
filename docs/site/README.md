# Noise Ordnance site page

This directory holds the Noise Ordnance page for
plugins.johnpeterman.me, moved here from the plugins site repo so
the page lives with the plugin. The design and styling are copied
unchanged from the site repo (radar theme); only the copy changed,
from the RNNoise engine to DeepFilterNet.

## File mapping (this dir -> site repo)

| Here | Site repo |
| --- | --- |
| `page.md` | `content/noise-ordnance/page.md` |
| `scaffold.css` | `scaffold/scaffold.css` |
| `override.css` | `content/noise-ordnance/override.css` |
| `topo.jpg` | `content/noise-ordnance/topo.jpg` |

The site build's `template.html`, `build.py`, `plugins.json`, and
`_redirects` stay in the site repo; a future submodule wiring will
point the build at this directory.

## Before / after audio (placeholders)

The Before / after section in `page.md` uses `<audio>` elements
whose `src` attributes point by relative path at the tests
directory (`../../tests/drills/`). Those files do not exist yet:
the follow-up commit (cleaned-up combo drill file + Jay's
before/after export) will supply them, and the filenames in the
page are provisional.

Note for go-live: the site build currently copies only top-level
media files from the page's own directory and warns on other
relative `src` paths. Either the build needs a relative-path rule
or the audio gets copied into the page directory when this page
goes live. Decision deferred to Jay.

When this page goes live, retire the fixed Before / after
placeholder block in the site repo's `template.html` in favor of
this page's own section, so the players don't appear twice.

## Live site status

Nothing here affects the live site. The plugins site repo still
serves the old RNNoise page until the submodule wiring lands and
this page replaces it.
