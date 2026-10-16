# MCP editing details

Read the published tool schema for exact selectors and arguments. The MCP
catalog uses consolidated controls and omits many native aliases.

## Project, track, and transport controls

- Update tools preflight all supplied fields, require at least one editable
  property, and leave omitted properties unchanged.
- `update_project` requires numerator/denominator together and uses
  `master_volume`/`master_pitch`.
- `create_track` requires `type:instrument|sample|automation|bb`; instrument
  options apply only to instrument tracks. `update_track` uses `fx_channel`
  for instrument mixer routing. Native sample tracks route to master only.
- `transport` requires `action:play|pause|stop|seek`; `position` is accepted
  and required only for seek. Play resumes a paused song; pause pauses active
  playback. Transport can dirty the song if playback lowers master volume
  to the configured cap.

## Notes and arrangement

`get_pattern` returns metadata and MIDI-key notes. Zero-volume notes are
omitted unless `include_disabled:true`. Other clip/track reads can request
notes explicitly. Note insertion returns only inserted notes and
`pattern_index` by default, rather than the whole pattern.

- `place_patterns_batch` preflights up to 128 song instrument placements,
  including source handles and names, before creating any clip.
- `set_notes_batch` preflights up to 512 unique current note indices. All
  indices refer to the ordering before the call, even if positions change.
- `replace_notes` replaces the full pattern with up to 512 notes; an empty
  list clears it. Use insertion batches for larger patterns when appropriate.
- Requests also have a 64-KiB UTF-8 limit. Split large batches by encoded
  request size as well as the tool's item limit. Reinspect indices after edits.

Pattern, automation, and sample duplication preserve session names,
mute/solo, auto-resize, and length, with an optional replacement `name`.
Stock LMMS 1.2.2 does not serialize sample clip names: those names last only
for the session. Inspect `name_persistent:false`, warnings, and
`get_lmms_info.capabilities` before depending on names after reload.

## Presets, samples, and VSTs

`load_instrument_file` accepts validated `.xpf` presets and supported
instrument files. Legacy presets, including Organic's `pad_ethereal.xpf`,
are upgraded natively before semantic validation.

`preserve_name`, `preserve_mix` (volume/pan/mixer routing), and
`preserve_instrument_settings` (base note/pitch/range) default to true.
Sample assignment preserves names and sample-clip length by default through
`preserve_name`/`preserve_length`. Inspect `side_effects` when loading still
changes settings.

Discovery tools such as `list_samples`, `list_presets`, and
`scan_vst_directory` support `search`, `offset`, and `limit`, stable path
ordering, and `pagination.total_count`/`next_offset`. MCP defaults to 50
entries; follow pagination when needed. Discovered paths and
`resolved_file_path` are absolute loader paths. Preset discovery reports
`compatible` and a reason when a preset is unavailable.

Use absolute local drive paths for project, preset, instrument, sample, VST,
and export operations. Project/preset inputs have a 64-MiB limit. Samples
have a 64-MiB encoded limit and decoded preflight limits of 300 seconds,
32 channels, and 256 MiB. Native DrumSynth `.ds` files bypass decoded-metadata
inspection.

Inspect `vst.plugin_loaded`, errors, and parameter support/count after VST
loading. SyndtSphere VST2/x64 loading, parameters, and isolated rendering are
covered by the regression suite when installed; other external VSTs and
full Zyn preset loading remain unverified. Discovery alone is insufficient.

## Parameters, mixing, effects, and automation

Parameters report `type`, `unit`, `path`, `writable`, `automation_capable`,
and enum `choices` where available. Edit the exact `value`; `display_value`
is formatted for reading. An unknown physical unit is labeled `native`.
LADSPA models distinguish processor, port, and control role; inactive/output
models are unwritable. VeSTige exposes normalized native automation models.
The stock instrument ABI does not expose VST vendor/version.

Use `get_mixer_channels` and `get_effect_chain` to inspect routing and
processing. Master removal and feedback sends are rejected. Mixer edits
report only other buses whose mute/solo states actually changed; request
broader detail when checking the whole mix.

Automation points use the parameter's native units and discrete, linear,
or cubic progression. `set_automation_points` defaults to `mode:"replace"`;
use `mode:"upsert"` to retain other positions. Confirm parameter writability
and automation support, and use the current project token for model writes.

## Response detail

`full_detail:true` includes related native detail. Explicit `include_notes`,
`include_clips`, `include_parameters`, `include_pattern`,
`include_all_channels`, and `include_all_effects` take precedence where
exposed. Avoid requesting entire related objects when the changed object
answers the question.
