---
name: lmms-operation-instructions
description: Inspect, compose, arrange, mix, and export music in the live LMMS 1.2.2 session through the LMMS MCP tools. Use for operating LMMS, including auditioning instruments and editing existing songs. Bridge implementation and regression testing use the repository development and testing documents.
---

# LMMS Operation Instructions

The LMMS MCP tools edit the open LMMS models, including unsaved changes.
Use them for live composition; do not substitute project-file rewrites or GUI
clicking for supported native edits. Tool schemas are the authority for exact
arguments and available controls.

## Connect and protect the session

1. Inspect the current session with `get_project_info` before replacing a song.
   Save unsaved work with `save_project` before new/load. Use
   `discard_unsaved:true` only when discarding those edits is intended.
2. Unless the user requests another level, use
   `update_project` with `master_volume:40` after connecting and after every
   `create_project` or `load_project`. Confirm the level before
   `transport` with `action:"play"`. This default limits unexpected playback
   levels; a saved volume does not establish the intended listening level.
3. Use an existing bridge-enabled session when available. The MCP server does
   not start or close LMMS. If launch is needed, first verify that no LMMS
   process is running, locate the installed bridge project, then run this from
   that project's root:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\start-lmms-with-bridge.ps1
   ```

   Add `-Project <absolute-file.mmp>` to open a saved song. Do not launch a
   second instance. Close an existing session only through `CloseMainWindow()`
   after saving and confirming that the window title has no `*`; never kill it.
   Preserve recovery files if startup encounters recovery data.

Use `get_lmms_info` to discover installed instruments, effects, resource roots,
and encoder capabilities. Use the published MCP tools rather than assuming
that every native command is exposed. Connection/server setup and the direct
pipe fallback are documented in [connection guidance](references/connection.md).

## Coordinates and selectors

- Timing is **48 ticks per quarter note**. A 4/4 bar is 192 ticks; account for
  the live time signature in other meters. Use ticks or bars for a coordinate,
  never both.
- All MCP `note.key` results and normal note inputs use MIDI pitches:
  **C4 = 60**, supported range 12..119. Native/legacy LMMS keys use C4 = 48;
  an explicit `lmms_key` input uses that native convention.
- Note positions are relative to their pattern. Clip positions are relative
  to the song. A note length of `-1` denotes a drum step.
- Prefer discovered `track_handle` and `clip_handle` over indices. Handles
  survive index shifts but expire on object deletion or project new/load;
  they cannot be reused in another session. Use either a track index or a
  track handle. Clip handles can target pattern, note, and automation edits.
- Parameter IDs are session-local. Reinspect after project reload or plugin
  replacement and include the discovered `project_token` on parameter writes.
- Track, master, and note volume use percent (0..200); mixer gain is a linear
  multiplier (0..2). Pan is -100..100. Instrument pitch uses cents; master
  pitch and pattern transposition use semitones.
- Song is the default container. Inner Beat/Bassline lanes use `container:"bb"`
  and `pattern_index` for the BB column. Track handles can select BB lanes
  without an index or explicit container. Place BB arrangement clips on song
  BB tracks with `place_bb_clip`.

## Compose and edit

- `create_project` starts empty by default. Use `update_project` for tempo,
  paired time-signature fields, and master settings.
- Create instrument, sample, automation, or BB tracks with `create_track`.
  Use `update_track`, `update_clip`, `update_mixer_channel`, and `update_effect`
  to change only supplied properties. Use `transport` for play/pause/stop/seek.
- Discover presets and samples with `list_presets` and `list_samples`; use the
  returned absolute paths. Discover VSTs with `scan_vst_directory` and load
  them with `add_vst_track`. Discovery does not establish compatibility.
- Use `get_pattern` to inspect notes, `add_notes_batch` to insert notes, and
  `set_notes_batch` or `replace_notes` for larger edits. Reinspect note indices
  after edits. Use `place_patterns_batch` for repeated song instrument clips.
- Read [editing details](references/editing.md) when working with batch limits,
  preset/sample preservation, parameters, mixer/effect routing, or automation.
- Save with `save_project` to an absolute local drive path. Replacing a
  different existing destination requires `overwrite:true`; saving clears the
  native dirty marker.

The runtime `update_project.playback_volume_cap` defaults to 40 percent and
can be configured when the user requests another level. New/load/play lowers
master volume when needed. Changing the cap alone does not dirty the song or
persist in the project; lowering the actual master volume changes song data.

## Inspect results and audition

`changed` reports data changed by this call; `modified` reports the current
song dirty state. Rejected preflight and no-op setters should leave song data
and dirty state unchanged. A native error can report `changed:true` after a
partial edit. A transport failure can report `changed:null` and
`outcome_indeterminate:true`. Inspect the affected objects before retrying;
edits are never retried automatically.

Edits return changed objects by default. Request `full_detail:true` or the
relevant `include_*` fields when related objects are needed. Verify the
musical result as well as command success. Compare rendered stem levels:
equal track percentages do not imply equal loudness across instruments.

The factory Zyn preset `Collection/0058-Warm Flute.xiz` was measured an octave
below its written pitches at the default MIDI base note 69. Setting its
track's `base_note` to MIDI 57 produced 349.24 Hz for F4 (MIDI 65) in an
isolated render. Check this preset's tuning when using it; the bridge's
MIDI-key convention remains unchanged.

Read [export instructions](references/export.md) before rendering audio or
exporting MIDI. Stop playback before rendering, track the returned `job_id`,
and require `job.state == "succeeded"` before treating an export as complete.
