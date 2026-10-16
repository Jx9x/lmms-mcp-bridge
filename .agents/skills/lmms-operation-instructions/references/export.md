# Audio and MIDI export

## Audio

`render_project` renders the current live song, including unsaved edits,
through native instruments, mixer, effects, and automation. Stop playback
before starting. Use an absolute local drive output path; replacing an
existing output requires `overwrite:true`.

The start reply is not completion evidence. Poll `render_status` with the
returned `job_id` and require `job.state == "succeeded"`. The job reports
frames, duration, bytes, peak, RMS, and failures. Alternatively, supply
`wait_seconds:0..30` to `render_project`. A timeout returns
`wait_timed_out:true` and current status; it neither cancels nor restarts
the render. Continue checking the same job.

Project edits, transport, and the main window are disabled while rendering.
Use `cancel_render` to cancel. Cancellation/failure preserves any previous
destination. Successful output is staged beside the destination and committed
with a Windows rename. Cleanup restores the audio device, quality,
parameters, playhead, arrangement length including muted tracks, window
state, and native dirty status.

| Format | Settings |
| --- | --- |
| WAV | 16/24-bit PCM or 32-bit float via `bit_depth` |
| FLAC | 16/24-bit via `bit_depth` |
| OGG/Vorbis | Check installed encoder availability |
| MP3 | `bitrate` and `quality` |

Defaults are 44.1-kHz stereo, native sinc-best interpolation without
oversampling, and a two-second tail. `tail_seconds` accepts 0..30. Renders
are capped at one hour / 2 GiB. Inspect `get_lmms_info` for actual installed
encoder availability.

Use peak/RMS and rendered audio to judge levels and verify sound, rather
than treating a successful request as proof of the intended musical result.

## MIDI

`export_midi` uses LMMS's native `midiexport` plugin. It exports instrument
and BB notes, static tempo, master pitch, and track-volume-based velocity.
It does not preserve samples, effects, parameter automation, mute/solo,
or MIDI program selection.

Empty projects are rejected. The native fixed buffer is guarded by a
conservative limit of 2000 expanded notes per instrument track and 1024
bytes per track name. Use an absolute local drive path and the documented
overwrite guard for an existing output.
