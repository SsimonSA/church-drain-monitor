# Claude Code notes for this repo

Stephen works on this project from two laptops that share the folder through Dropbox.
Claude's own session history and memory do **not** sync between them, so anything that
must survive a laptop switch has to live in this repo.

## Read first

- `firmware/docs/field-log.md` — running log of site visits and what happened. This is
  the source of truth for "where did we leave off". Read it before answering that question.
- `firmware/docs/field-visit-checklist.md` — the pre-flight for a visit.
- `README.md` — how the firmware, OTA and store-and-forward work.

## Keep the log current

Whenever a session touches any of these, append an entry to `firmware/docs/field-log.md`
before the session ends, and remind Stephen to commit it:

- a site visit (planned, done, or reported after the fact)
- a firmware release or OTA manifest change
- a change to what is deployed, what is broken, or what is next

An entry is a dated heading, what was done, what was observed, and what is still open.
Keep it short. If the outcome is unknown, say so rather than leaving the entry out.

## Conventions

- Firmware lives in `firmware/`; `FW_VERSION` must be bumped on every release and the
  OTA manifest must exceed the running version for the device to update.
- The field unit has no WiFi of its own. Updates only land on a visit with a phone hotspot.
  USB reflash on site is the only rollback.
- `secrets.h` is gitignored and must never be committed.
