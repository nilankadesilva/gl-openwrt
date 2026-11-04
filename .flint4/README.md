# .flint4: CI helpers for the Flint 4 (GL-BE14000) integration branch

- `manifest.txt`: package manifest of the last flashed image (refreshed by flint4-stage.sh --promote). Drives the feed filter of the watch.
- `watch.sh` + workflow "flint4 watch": daily, compares upstream main and the feeds with the last image tag, posts a report to the
  issue labelled `flint4-watch`. No build.
- workflow "flint4 prepare branch": on demand, rebases the local stack onto upstream main as `flint4-main-rN` and pushes it, or
  reports the conflicting commit. The kernel-patch refresh and the build happen locally (`flint4-rebuild.sh N --base mine/flint4-main-rN`).
Flashing is always a manual decision.
