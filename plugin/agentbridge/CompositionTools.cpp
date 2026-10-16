#include "CompositionCommands.h"
#include "CommandSchema.h"
#include "MusicTheory.h"
#include "Song.h"
#include <QtCore/QJsonDocument>

namespace
{

QJsonObject extend( QJsonObject base, std::initializer_list<std::pair<const char *, QJsonObject> > fields )
{
	for( const auto & field : fields ) { base[field.first] = field.second; }
	return base;
}

QJsonObject choice( std::initializer_list<QJsonObject> options )
{
	QJsonArray array;
	for( const QJsonObject & option : options ) { array.append( option ); }
	QJsonObject o;
	o["oneOf"] = array;
	return o;
}

QJsonObject requiredFields( const QStringList & fields )
{
	QJsonArray a; for( const QString & field : fields ) { a.append( field ); }
	return QJsonObject{ { "required", a } };
}

void requireAny( QJsonObject & schema, const QStringList & fields )
{
	QJsonArray options; for( const QString & field : fields ) { options.append( requiredFields( { field } ) ); }
	QJsonArray rules = schema["allOf"].toArray();
	rules.append( QJsonObject{ { "anyOf", options } } ); schema["allOf"] = rules;
}

void exclusive( QJsonObject & schema, const QStringList & fields )
{
	QJsonArray rules = schema["allOf"].toArray();
	for( int i = 0; i < fields.size(); ++i ) { for( int j = i + 1; j < fields.size(); ++j )
	{
		rules.append( QJsonObject{ { "not", requiredFields( { fields[i], fields[j] } ) } } );
	} }
	schema["allOf"] = rules;
}

QJsonObject integers( std::initializer_list<int> values )
{
	QJsonArray a; for( int value : values ) { a.append( value ); }
	return QJsonObject{ { "type", "integer" }, { "enum", a } };
}

}

QJsonArray CompositionCommands::tools()
{
	static QJsonArray cached;
	if( !cached.isEmpty() ) { return cached; }
	using namespace CommandSchema;
	QJsonArray a;
	auto add = [&a]( const char * name, const char * description, const QJsonObject & fields = QJsonObject(),
		const QStringList & required = QStringList(), bool readOnly = false, bool destructive = false )
	{
		a.append( tool( name, description, object( fields, required ), readOnly, destructive ) );
	};
	const QJsonObject index = number( 0, 9999, true );
	const QJsonObject tick = number( 0, MaxSongLength, true );
	const QJsonObject bars = number( 0, MaxSongLength );
	const QJsonObject modelId = number( 1, 4294967295.0, true );
	const QJsonObject value = choice( { number( -1e9, 1e9 ), boolean() } );
	const QJsonObject scope = properties( {{ "container", text( { "song", "bb" } ) }} );
	const QJsonObject track = extend( scope, {{ "track_index", index }} );
	const QJsonObject pattern = extend( track, {{ "pattern_index", index }, { "bb_instrument_index", index }} );
	const QJsonObject clip = extend( track, {{ "clip_index", index }, { "pos_bars", bars }} );
	const QJsonObject target = extend( scope, {{ "target_type", text( { "track", "mixer" } ) }, { "target_index", index }} );
	const QJsonObject pitch = choice( { number( 12, 119, true ), text( QStringList(), 12 ) } );
	const QJsonObject note = properties( {{ "key", pitch }, { "note_name", text( QStringList(), 12 ) },
		{ "lmms_key", number( 0, 107, true ) }, { "pos", tick },
		{ "length", choice( { number( 1, MaxSongLength, true ), number( -1, -1, true ) } ) },
		{ "volume", number( 0, 200, true ) }, { "panning", number( -100, 100, true ) }} );
	const QJsonObject points = array( object( properties( {{ "pos", tick }, { "value", number( -1e9, 1e9 ) }} ), { "pos", "value" } ) );
	for( const char * name : { "help", "ping", "song", "selftest", "get_project_info", "get_project_xml", "get_lmms_info", "list_available_plugins" } )
	{
		add( name, "Inspect the live LMMS session without editing it.", QJsonObject(), QStringList(), true );
	}
	for( const char * name : { "tracks", "get_tracks" } ) { add( name, "Inspect song and BB tracks, including live clips and notes.", QJsonObject(), QStringList(), true ); }
	for( const char * name : { "list_tracks", "get_arrangement" } ) { add( name, "Inspect tracks/clips in the selected container (song by default).", scope, QStringList(), true ); }
	for( const char * name : { "tempo", "set_tempo" } ) { add( name, "Set integer BPM and mark the native project modified.", properties( {{ "bpm", number( MinTempo, MaxTempo, true ) }} ), { "bpm" } ); }
	for( const char * name : { "play", "stop", "pause", "show" } ) { add( name, "Control transport or the bridge window without editing project data." ); }
	add( "set_playhead", "Seek the song transport in ticks; not a project edit.", properties( {{ "position", tick }} ), { "position" } );
	add( "project.new", "Create LMMS's default project. Unsaved changes require explicit discard_unsaved=true.", properties( {{ "discard_unsaved", boolean() }} ), QStringList(), false, true );
	add( "create_project", "Create a native project (empty by default). Name is a hint; LMMS derives its title from the saved filename. Unsaved changes need explicit discard_unsaved=true.",
		properties( {{ "discard_unsaved", boolean() }, { "empty", boolean() }, { "bpm", number( MinTempo, MaxTempo, true ) }, { "name", text( QStringList(), 256 ) }} ), QStringList(), false, true );
	add( "load_project", "Load a validated .mmp/.mmpz through LMMS. Unsaved changes need explicit discard_unsaved=true.", properties( {{ "path", text() }, { "discard_unsaved", boolean() }} ), { "path" }, false, true );
	add( "save_project", "Native save clears modified state. Existing destinations need overwrite=true unless saving the current project.", properties( {{ "path", text() }, { "overwrite", boolean() }} ), QStringList(), false, true );
	add( "set_time_signature", "Set the native meter; ticks per quarter remain 48.", properties( {{ "numerator", number( 1, 32, true ) }, { "denominator", number( 1, 32, true ) }} ), { "numerator", "denominator" } );
	add( "set_master_volume", "Set master gain in percent (0..200).", properties( {{ "volume", number( 0, 200, true ) }} ), { "volume" } );
	add( "set_master_pitch", "Set master transposition in semitones (-12..12).", properties( {{ "pitch", number( -12, 12, true ) }} ), { "pitch" } );
	add( "add_instrument_track", "Create a native instrument track; instrument defaults to tripleoscillator. Use container=bb for a BB lane.",
		extend( scope, {{ "name", text( QStringList(), 256 ) }, { "instrument", text() }, { "volume", number( 0, 200 ) }, { "panning", number( -100, 100 ) }, { "fx_channel", index }} ) );
	for( const char * name : { "add_sample_track", "add_automation_track", "add_pattern_track" } ) { add( name, "Create a native sample, automation, or BB track.", extend( scope, {{ "name", text( QStringList(), 256 ) }} ) ); }
	add( "get_track", "Inspect a native track, clips, and parameter identifiers.", track, { "track_index" }, true );
	add( "remove_track", "Delete a native track and its clips; indices can shift.", track, { "track_index" }, false, true );
	add( "clone_track", "Clone a native track and its contents.", extend( track, {{ "name", text( QStringList(), 256 ) }} ), { "track_index" } );
	add( "rename_track", "Rename a native track.", extend( track, {{ "name", text( QStringList(), 256 ) }} ), { "track_index", "name" } );
	add( "set_track_volume", "Set instrument/sample track gain in percent.", extend( track, {{ "volume", number( 0, 200 ) }} ), { "track_index", "volume" } );
	add( "set_track_panning", "Set instrument/sample pan (-100 left, +100 right).", extend( track, {{ "panning", number( -100, 100 ) }} ), { "track_index", "panning" } );
	add( "mute_track", "Set native track mute.", extend( track, {{ "muted", boolean() }} ), { "track_index" } );
	add( "solo_track", "Set native track solo.", extend( track, {{ "solo", boolean() }} ), { "track_index" } );
	add( "load_instrument", "Replace an instrument using LMMS's live plugin registry.", extend( track, {{ "instrument", text() }} ), { "track_index", "instrument" } );
	add( "set_instrument_settings", "Set base note (MIDI), pitch cents, bend range, and mixer assignment.", extend( track, {{ "base_note", number( 12, 119, true ) }, { "pitch", number( -2400, 2400 ) }, { "pitch_range", number( 1, 24, true ) }, { "fx_channel", index }} ), { "track_index" } );
	const QJsonObject parameterTarget = extend( scope, {{ "target_type", text( { "song", "track", "instrument", "mixer", "effect" } ) }, { "target_index", index },
		{ "effect_index", index }, { "effect_target_type", text( { "track", "mixer" } ) }} );
	add( "get_parameters", "Discover native parameter names, model_ids, units/ranges, and current values. IDs are session-local; reinspect after project/plugin replacement.", parameterTarget, QStringList(), true );
	add( "set_parameter", "Set a currently owned native parameter by model_id. Values are native parameter units.", properties( {{ "model_id", modelId }, { "value", value }} ), { "model_id", "value" } );
	add( "set_parameters", "Validate all parameter edits before applying the batch.", properties( {{ "values", array( object( properties( {{ "model_id", modelId }, { "value", value }} ), { "model_id", "value" } ), 512, 1 ) }} ), { "values" } );
	add( "list_presets", "List installed/user LMMS .xpf/.xiz presets, optionally filtered by instrument or category.", properties( {{ "instrument", text() }, { "category", text() }} ), QStringList(), true );
	add( "list_zyn_presets", "List installed/user ZynAddSubFX .xiz/.xpf presets.", properties( {{ "category", text() }} ), QStringList(), true );
	add( "load_preset", "Load a validated native .xpf preset without replacing the track's notes/clips.", extend( track, {{ "path", text() }, { "preserve_name", boolean() }} ), { "track_index", "path" } );
	add( "load_zyn_preset", "Load an absolute .xiz or Zyn .xpf through the native instrument.", extend( track, {{ "preset", text() }} ), { "track_index", "preset" } );
	QJsonObject zyn = track;
	for( const char * name : { "portamento", "filterfreq", "filterq", "bandwidth", "fm_gain", "res_centerfreq", "res_bandwidth" } ) { zyn[name] = number( 0, 127, true ); }
	add( "set_zyn_params", "Set ZynAddSubFX's native exposed MIDI-CC knobs (0..127).", zyn, { "track_index" } );
	add( "load_instrument_file", "Load a validated .xpf preset or an absolute sample, soundfont, GIG, or XIZ supported by the current instrument. Notes/clips are preserved; preserve_name defaults to true.", extend( track, {{ "path", text() }, { "preserve_name", boolean() }} ), { "track_index", "path" } );
	add( "scan_vst_directory", "List x64 VST2 DLL candidates without loading them; LMMS's own plugins are not VSTs.", properties( {{ "directory", text() }, { "recursive", boolean() }} ), { "directory" }, true );
	add( "add_vst_track", "Load an x64 VST2 DLL with native VeSTige; availability and compatibility depend on the external plugin.", extend( scope, {{ "name", text( QStringList(), 256 ) }, { "plugin_path", text() }} ), { "plugin_path" } );
	for( const char * name : { "get_pattern", "get_notes" } ) { add( name, "Inspect a note pattern. get_notes uses MIDI keys (C4=60) and stable current note indices.", extend( pattern, {{ "include_disabled", boolean() }} ), { "track_index" }, true ); }
	QJsonObject noteArgs = pattern;
	for( auto it = note.begin(); it != note.end(); ++it ) { noteArgs[it.key()] = it.value(); }
	for( const char * name : { "add_note", "add_note_by_name" } ) { add( name, "Add a note using MIDI key 12..119 or note_name (C4=60). pos/length are ticks relative to pattern; 48 ticks is a quarter note. length=-1 is a drum step.", noteArgs, { "track_index" } ); }
	add( "add_notes_batch", "Validate every note before inserting the batch; pattern is extended to contain them. include_pattern defaults to true for native compatibility.", extend( pattern, {{ "notes", array( object( note ), 512, 1 ) }, { "include_pattern", boolean() }} ), { "track_index", "notes" } );
	add( "set_note", "Edit a current note index and extend/sort its native pattern.", extend( noteArgs, {{ "note_index", index }} ), { "track_index", "note_index" } );
	add( "remove_note", "Delete one native note; indices can shift.", extend( pattern, {{ "note_index", index }} ), { "track_index", "note_index" }, false, true );
	add( "clear_notes", "Clear the pattern's native notes.", pattern, { "track_index" }, false, true );
	add( "transpose_pattern", "Transpose all pattern notes by semitones, rejecting out-of-range results atomically.", extend( pattern, {{ "semitones", number( -107, 107, true ) }} ), { "track_index", "semitones" } );
	const QJsonObject placement = extend( track, {{ "pos_ticks", tick }, { "pos_bars", bars }, { "length_ticks", tick }, { "length_bars", bars }, { "name", text( QStringList(), 256 ) }} );
	add( "place_pattern", "Place an empty native instrument pattern, or copy source_pattern_index on the same track.", extend( placement, {{ "source_pattern_index", index }} ), { "track_index" } );
	add( "place_bb_clip", "Place a native BB arrangement clip on a BB song track.", placement, { "track_index" } );
	add( "place_sample_clip", "Decode and place an audio file as a native sample clip.", extend( placement, {{ "file_path", text() }} ), { "track_index", "file_path" } );
	add( "assign_sample_file", "Assign audio to a sample clip or AudioFileProcessor instrument.", extend( clip, {{ "file_path", text() }} ), { "track_index", "file_path" } );
	add( "get_clip", "Inspect a clip by index or unique song position.", clip, { "track_index" }, true );
	add( "rename_clip", "Rename a native clip.", extend( clip, {{ "name", text( QStringList(), 256 ) }} ), { "track_index", "name" } );
	add( "move_clip", "Move a native clip using ticks or bars; select by clip_index or old_pos_bars.", extend( track, {{ "clip_index", index }, { "old_pos_bars", bars }, { "new_pos_ticks", tick }, { "new_pos_bars", bars }} ), { "track_index" } );
	add( "resize_clip", "Resize a native clip in ticks or bars.", extend( clip, {{ "length_ticks", tick }, { "length_bars", bars }} ), { "track_index" } );
	add( "duplicate_clip", "Duplicate a native clip on the same track.", extend( clip, {{ "new_pos_ticks", tick }, { "new_pos_bars", bars }} ), { "track_index" } );
	add( "delete_clip", "Delete a native clip; indices can shift.", clip, { "track_index" }, false, true );
	add( "add_mixer_channel", "Create a native mixer channel; volume is a linear multiplier (0..2).", properties( {{ "name", text( QStringList(), 256 ) }, { "volume", number( 0, 2 ) }} ) );
	add( "get_mixer_channels", "Inspect native channels, routing, effects, and volume model_ids.", QJsonObject(), QStringList(), true );
	const QJsonObject channel = properties( {{ "channel_num", index }} );
	add( "remove_mixer_channel", "Delete a non-master native channel and repair instrument routing.", channel, { "channel_num" }, false, true );
	add( "set_mixer_channel_volume", "Set channel gain as a linear multiplier (0..2).", extend( channel, {{ "volume", number( 0, 2 ) }} ), { "channel_num", "volume" } );
	add( "set_mixer_channel_name", "Rename a native mixer channel.", extend( channel, {{ "name", text( QStringList(), 256 ) }} ), { "channel_num", "name" } );
	add( "mute_mixer_channel", "Set native mixer mute.", extend( channel, {{ "muted", boolean() }} ), { "channel_num" } );
	add( "solo_mixer_channel", "Set native mixer solo.", extend( channel, {{ "solo", boolean() }} ), { "channel_num" } );
	add( "assign_track_to_mixer", "Route an instrument track to an existing native mixer channel. LMMS 1.2.2 sample tracks route to master only.", extend( track, {{ "channel_num", index }} ), { "track_index", "channel_num" } );
	const QJsonObject routing = properties( {{ "from_channel", index }, { "to_channel", index }} );
	add( "set_mixer_send", "Create/update a send, rejecting feedback loops.", extend( routing, {{ "amount", number( 0, 1 ) }} ), { "from_channel", "to_channel" } );
	add( "remove_mixer_send", "Remove a native mixer send.", routing, { "from_channel", "to_channel" }, false, true );
	add( "get_effect_chain", "Inspect native track/mixer effects in processing order.", target, { "target_type", "target_index" }, true );
	add( "list_effect_plugins", "List installed native effects and their required subplugin keys.", QJsonObject(), QStringList(), true );
	add( "list_effect_subplugins", "Discover LADSPA/other keys required by add_effect.", properties( {{ "effect_name", text() }} ), { "effect_name" }, true );
	add( "add_effect", "Instantiate a native effect on a track or mixer. For LADSPA select a key from list_effect_subplugins.", extend( target, {{ "effect_name", text() }, { "subplugin_index", index }, { "wet", number( -1, 1 ) }} ), { "target_type", "target_index", "effect_name" } );
	const QJsonObject effect = extend( target, {{ "effect_index", index }} );
	add( "remove_effect", "Remove a native effect in chain order.", effect, { "target_type", "target_index", "effect_index" }, false, true );
	add( "toggle_effect", "Enable/bypass a native effect.", extend( effect, {{ "enabled", boolean() }} ), { "target_type", "target_index", "effect_index" } );
	add( "set_effect_wet", "Set native dry/wet level (-1..1).", extend( effect, {{ "wet", number( -1, 1 ) }} ), { "target_type", "target_index", "effect_index", "wet" } );
	add( "set_effect_chain_enabled", "Enable/bypass the entire native effect chain.", extend( target, {{ "enabled", boolean() }} ), { "target_type", "target_index", "enabled" } );
	const QJsonObject automation = extend( placement, {{ "model_id", modelId }, { "target_track_index", index }, { "target_parameter", text( { "volume", "panning", "pitch" } ) },
		{ "points", points }, { "progression", text( { "discrete", "linear", "cubic" } ) }, { "tension", number( 0, 1 ) }} );
	add( "add_automation", "Create a native automation clip linked to model_id or a track parameter. Point values use native parameter units; positions are relative ticks.", automation, { "track_index", "points" } );
	add( "get_automation", "Inspect linked models, interpolation, and native points.", clip, { "track_index" }, true );
	add( "set_automation_points", "Validate all points before editing a native automation clip. mode=replace (default) replaces all points; mode=upsert inserts/updates supplied positions without removing others.", extend( clip, {{ "points", points }, { "mode", text( { "replace", "upsert" } ) }, { "progression", text( { "discrete", "linear", "cubic" } ) }, { "tension", number( 0, 1 ) }} ), { "track_index", "points" } );
	add( "set_automation_point", "Insert/update one native automation point.", extend( clip, {{ "pos", tick }, { "value", number( -1e9, 1e9 ) }} ), { "track_index", "pos", "value" } );
	add( "remove_automation_point", "Delete a native automation point.", extend( clip, {{ "pos", tick }} ), { "track_index", "pos" }, false, true );
	add( "link_automation", "Link another compatible native parameter to a clip.", extend( clip, {{ "model_id", modelId }} ), { "track_index", "model_id" } );
	add( "note_name_to_key", "Convert a standard note name to MIDI key (C4=60); does not edit the song.", properties( {{ "name", text( QStringList(), 12 ) }} ), { "name" }, true );
	add( "key_to_note_name", "Convert a MIDI key to a standard sharp note name.", properties( {{ "key", number( 0, 127, true ) }} ), { "key" }, true );
	add( "bars_to_ticks_converter", "Convert bars using the live native time signature (48 ticks per quarter).", properties( {{ "bars", bars }} ), { "bars" }, true );
	add( "ticks_to_bars_converter", "Convert ticks using the live time signature.", properties( {{ "ticks", tick }} ), { "ticks" }, true );
	add( "generate_scale", "Generate MIDI pitches and names without editing the project.", properties( {{ "root", text( QStringList(), 12 ) }, { "scale_type", text( MusicTheory::scales().keys() ) },
		{ "octave", number( -1, 9, true ) }, { "num_octaves", number( 1, 8, true ) }} ), { "root" }, true );
	add( "render_project", "Render the LIVE song asynchronously; poll render_status until job.state=succeeded. Playback must be stopped; edits/transport are blocked while rendering. Native sinc-best interpolation, no oversampling. bit_depth applies to WAV/FLAC; bitrate/quality are MP3-only. Tail defaults to 2 seconds. One-hour/2-GiB limit. Existing output needs overwrite=true.",
		properties( {{ "path", text() }, { "format", text( { "wav", "ogg", "flac", "mp3" } ) }, { "sample_rate", number( 22050, 192000, true ) }, { "bit_depth", number( 16, 32, true ) },
			{ "bitrate", number( 32, 320, true ) }, { "quality", number( 0, 9, true ) }, { "tail_seconds", number( 0, 30 ) }, { "overwrite", boolean() }} ), { "path" }, false, true );
	add( "render_status", "Inspect the asynchronous native rendering job.", QJsonObject(), QStringList(), true );
	add( "cancel_render", "Cancel rendering without replacing its destination file." );
	add( "export_midi", "Export native instrument/BB notes to a standard MIDI file. Does not export samples/audio/effects. Existing output needs overwrite=true.", properties( {{ "path", text() }, { "overwrite", boolean() }} ), { "path" }, false, true );
	add( "create_track", "Create an instrument, sample, automation, or BB track. Instrument options apply only to instrument tracks; instrument defaults to tripleoscillator. container=bb creates an inner lane; BB arrangement tracks require song.",
		extend( scope, {{ "type", text( { "instrument", "sample", "automation", "bb" } ) }, { "name", text( QStringList(), 256 ) }, { "instrument", text() }, { "volume", number( 0, 200 ) }, { "panning", number( -100, 100 ) }, { "fx_channel", index }} ), { "type" } );
	add( "update_project", "Update BPM, meter (numerator and denominator together), master_volume percent, or master_pitch semitones. Supply at least one property; all edits are preflighted and omitted properties stay unchanged.",
		properties( {{ "bpm", number( MinTempo, MaxTempo, true ) }, { "numerator", number( 1, 32, true ) }, { "denominator", number( 1, 32, true ) }, { "master_volume", number( 0, 200, true ) }, { "master_pitch", number( -12, 12, true ) }} ) );
	add( "update_track", "Update name, percent volume, panning (-100..100), mute, solo, or fx_channel routing. Supply at least one property; all edits are preflighted. Routing requires an instrument track; sample tracks route to master only.",
		extend( track, {{ "name", text( QStringList(), 256 ) }, { "volume", number( 0, 200 ) }, { "panning", number( -100, 100 ) }, { "muted", boolean() }, { "solo", boolean() }, { "fx_channel", index }} ), { "track_index" } );
	add( "update_clip", "Update a clip's name, position, or length together; supply at least one property. Select by clip_index or pos_bars/old_pos_bars. Coordinates use ticks or bars, never both; all edits are preflighted.",
		extend( clip, {{ "old_pos_bars", bars }, { "name", text( QStringList(), 256 ) }, { "new_pos_ticks", tick }, { "new_pos_bars", bars }, { "length_ticks", tick }, { "length_bars", bars }} ), { "track_index" } );
	add( "update_mixer_channel", "Update channel name, volume (linear gain 0..2), mute, or solo. Supply at least one property; all edits are preflighted and omitted properties stay unchanged.",
		extend( channel, {{ "name", text( QStringList(), 256 ) }, { "volume", number( 0, 2 ) }, { "muted", boolean() }, { "solo", boolean() }} ), { "channel_num" } );
	add( "update_effect", "Update effect enabled/bypassed state or wet level (-1..1). Supply at least one property; all edits are preflighted and omitted properties stay unchanged.",
		extend( effect, {{ "enabled", boolean() }, { "wet", number( -1, 1 ) }} ), { "target_type", "target_index", "effect_index" } );
	add( "transport", "Play/resume, pause, stop, or seek without editing the project. action=seek requires position in ticks; other actions do not accept position.",
		properties( {{ "action", text( { "play", "pause", "stop", "seek" } ) }, { "position", tick }} ), { "action" } );
	add( "list_samples", "Discover factory/user samples with readable absolute paths; filter by category or search and paginate with offset/limit.", properties( {{ "category", text() }} ), QStringList(), true );
	QJsonObject batchPlacement = object( extend( placement, {{ "track_handle", text( QStringList(), 160 ) }, { "source_pattern_index", index }, { "source_pattern_handle", text( QStringList(), 160 ) }} ) );
	requireAny( batchPlacement, { "track_index", "track_handle" } );
	exclusive( batchPlacement, { "track_index", "track_handle" } );
	for( const QStringList & pair : { QStringList{ "pos_ticks", "pos_bars" }, QStringList{ "length_ticks", "length_bars" }, QStringList{ "source_pattern_index", "source_pattern_handle" } } ) { exclusive( batchPlacement, pair ); }
	add( "place_patterns_batch", "Preflight every placement, then place/copy instrument patterns. Handles survive index shifts and expire on project replacement/deletion.", properties( {{ "placements", array( batchPlacement, 128, 1 ) }} ), { "placements" } );
	QJsonObject batchNote = object( extend( note, {{ "note_index", index }} ), { "note_index" } );
	requireAny( batchNote, { "key", "note_name", "lmms_key", "pos", "length", "volume", "panning" } );
	exclusive( batchNote, { "key", "note_name", "lmms_key" } );
	add( "set_notes_batch", "Preflight unique current note indices before editing/sorting the pattern. Edits all refer to the original ordering of this call.", extend( pattern, {{ "edits", array( batchNote, 512, 1 ) }} ), { "track_index", "edits" } );
	QJsonObject replacementNote = object( note ); exclusive( replacementNote, { "key", "note_name", "lmms_key" } );
	add( "replace_notes", "Preflight and replace all notes in a pattern; an empty list clears it. Returns the replacement notes. No-op replacement leaves the dirty state unchanged.", extend( pattern, {{ "notes", array( replacementNote, 512 ) }} ), { "track_index", "notes" }, false, true );

	// Publish the same conditional constraints that native preflight enforces.
	// Keep index selectors for compatibility while permitting discovered handles.
	QJsonArray enriched;
	for( int i = 0; i < a.size(); ++i )
	{
		QJsonObject entry = a[i].toObject();
		// Qt 5.4 nested values share the catalog's binary allocation. Repeated
		// edits to a nested object can grow that allocation past its offset limit.
		// Detach each schema before enriching it, and cache the finished catalog.
		QJsonObject schema = QJsonDocument::fromJson( QJsonDocument( entry["inputSchema"].toObject() ).toJson( QJsonDocument::Compact ) ).object();
		QJsonObject props = QJsonDocument::fromJson( QJsonDocument( schema["properties"].toObject() ).toJson( QJsonDocument::Compact ) ).object();
		const QString name = entry["name"].toString();
		props["project_token"] = text( QStringList(), 64 );
		if( props.contains( "track_index" ) )
		{
			props["track_handle"] = text( QStringList(), 160 );
			QStringList selectors{ "track_index", "track_handle" };
			if( props.contains( "pattern_index" ) || props.contains( "clip_index" ) || props.contains( "old_pos_bars" ) ) { props["clip_handle"] = text( QStringList(), 160 ); selectors << "clip_handle"; }
			QJsonArray required = schema["required"].toArray();
			for( int j = required.size() - 1; j >= 0; --j ) { if( required[j].toString() == "track_index" ) { required.removeAt( j ); } }
			schema["required"] = required; requireAny( schema, selectors );
			exclusive( schema, { "track_index", "track_handle" } );
		}
		if( props.contains( "target_type" ) )
		{
			props["track_handle"] = text( QStringList(), 160 );
			QJsonArray required = schema["required"].toArray();
			for( int j = required.size() - 1; j >= 0; --j ) { if( required[j].toString() == "target_index" ) { required.removeAt( j ); } }
			schema["required"] = required;
			QJsonObject conditional;
			conditional["if"] = QJsonObject{ { "properties", properties( {{ "target_type", text( { "mixer" } ) }} ) }, { "required", QJsonArray() << "target_type" } };
			conditional["then"] = QJsonObject{ { "required", QJsonArray() << "target_index" }, { "not", requiredFields( { "track_handle" } ) } };
			QJsonObject trackTarget; requireAny( trackTarget, { "target_index", "track_handle" } );
			conditional["else"] = trackTarget;
			if( name == "get_parameters" )
			{
				QJsonObject inspect;
				inspect["if"] = QJsonObject{ { "properties", properties( {{ "target_type", text( { "song" } ) }} ) }, { "required", QJsonArray() << "target_type" } };
				inspect["else"] = conditional; conditional = inspect;
			}
			QJsonArray rules = schema["allOf"].toArray(); rules.append( conditional ); schema["allOf"] = rules;
			exclusive( schema, { "target_index", "track_handle" } );
		}
		if( name == "place_pattern" ) { props["source_pattern_handle"] = text( QStringList(), 160 ); }
		if( name == "duplicate_clip" ) { props["name"] = text( QStringList(), 256 ); }
		if( name == "get_mixer_channels" ) { props["channel_num"] = index; }
		if( name == "render_status" ) { props["job_id"] = number( 1, 9007199254740991.0, true ); }
		if( name == "list_presets" || name == "list_zyn_presets" || name == "list_samples" || name == "scan_vst_directory" )
		{
			props["search"] = text( QStringList(), 256 ); props["offset"] = number( 0, 10000, true ); props["limit"] = number( 1, 5000, true );
		}
		if( name == "load_preset" || name == "load_instrument_file" || name == "load_zyn_preset" )
		{
			props["preserve_name"] = boolean(); props["preserve_mix"] = boolean(); props["preserve_instrument_settings"] = boolean();
			entry["description"] = entry["description"].toString() + " Name, mix and base-note/pitch settings are preserved by default; preservation can be disabled. side_effects reports remaining changes.";
		}
		if( name == "assign_sample_file" ) { props["preserve_name"] = boolean(); props["preserve_length"] = boolean(); }
		if( name == "update_project" )
		{
			props["playback_volume_cap"] = number( 0, 200, true );
			schema["dependencies"] = QJsonObject{ { "numerator", QJsonArray() << "denominator" }, { "denominator", QJsonArray() << "numerator" } };
			requireAny( schema, { "bpm", "numerator", "denominator", "master_volume", "master_pitch", "playback_volume_cap" } );
		}
		if( props.contains( "denominator" ) ) { props["denominator"] = integers( { 1, 2, 4, 8, 16, 32 } ); }
		if( props.contains( "bit_depth" ) )
		{
			props["bit_depth"] = integers( { 16, 24, 32 } );
			QJsonArray rules = schema["allOf"].toArray();
			rules.append( QJsonObject{ { "if", QJsonObject{ { "properties", properties( {{ "format", text( { "flac" } ) }} ) }, { "required", QJsonArray() << "format" } } }, { "then", QJsonObject{ { "properties", properties( {{ "bit_depth", integers( { 16, 24 } ) }} ) } } } } );
			schema["allOf"] = rules;
		}
		if( name == "update_track" ) { requireAny( schema, { "name", "volume", "panning", "muted", "solo", "fx_channel" } ); }
		if( name == "update_clip" ) { requireAny( schema, { "name", "new_pos_ticks", "new_pos_bars", "length_ticks", "length_bars" } ); }
		if( name == "update_mixer_channel" ) { requireAny( schema, { "name", "volume", "muted", "solo" } ); }
		if( name == "update_effect" ) { requireAny( schema, { "enabled", "wet" } ); }
		if( name == "set_instrument_settings" ) { requireAny( schema, { "base_note", "pitch", "pitch_range", "fx_channel" } ); }
		if( name == "set_note" ) { requireAny( schema, { "key", "note_name", "lmms_key", "pos", "length", "volume", "panning" } ); }
		if( name == "transport" )
		{
			schema["if"] = QJsonObject{ { "properties", properties( {{ "action", text( { "seek" } ) }} ) } };
			schema["then"] = requiredFields( { "position" } ); schema["else"] = QJsonObject{ { "not", requiredFields( { "position" } ) } };
		}
		if( name == "create_track" )
		{
			schema["if"] = QJsonObject{ { "properties", properties( {{ "type", text( { "instrument" } ) }} ) } };
			QJsonObject other; for( const QString & field : QStringList{ "instrument", "volume", "panning", "fx_channel" } )
			{
				QJsonArray rules = other["allOf"].toArray(); rules.append( QJsonObject{ { "not", requiredFields( { field } ) } } ); other["allOf"] = rules;
			}
			schema["else"] = other;
		}
		for( const QStringList & pair : { QStringList{ "pos_ticks", "pos_bars" }, QStringList{ "length_ticks", "length_bars" }, QStringList{ "new_pos_ticks", "new_pos_bars" }, QStringList{ "source_pattern_index", "source_pattern_handle" } } )
		{ if( props.contains( pair[0] ) && props.contains( pair[1] ) ) { exclusive( schema, pair ); } }
		if( props.contains( "key" ) && props.contains( "note_name" ) ) { exclusive( schema, { "key", "note_name", "lmms_key" } ); }
		if( name == "add_notes_batch" )
		{
			QJsonObject notes = props["notes"].toObject(), item = notes["items"].toObject(); exclusive( item, { "key", "note_name", "lmms_key" } ); notes["items"] = item; props["notes"] = notes;
		}
		if( name == "list_tracks" || name == "get_track" || name == "get_arrangement" || props.contains( "track_index" ) || name == "create_track" || name == "place_patterns_batch" || name.startsWith( "add_" ) || name.contains( "effect" ) || name.contains( "mixer" ) || name == "get_pattern" )
		{
			props["include_notes"] = boolean(); props["include_parameters"] = boolean(); props["include_clips"] = boolean();
		}
		if( name == "set_note" || name == "set_notes_batch" || name == "replace_notes" || name == "remove_note" || name == "clear_notes" || name == "transpose_pattern" || name == "add_note" || name == "add_note_by_name" ) { props["include_pattern"] = boolean(); }
		if( name.contains( "mixer" ) && name != "get_mixer_channels" ) { props["include_all_channels"] = boolean(); }
		if( name == "add_effect" || name == "remove_effect" || name == "update_effect" || name == "toggle_effect" || name == "set_effect_wet" || name == "set_effect_chain_enabled" ) { props["include_all_effects"] = boolean(); }
		schema["properties"] = props;
		entry["inputSchema"] = schema;
		enriched.append( entry );
	}
	cached = enriched;
	return cached;
}
