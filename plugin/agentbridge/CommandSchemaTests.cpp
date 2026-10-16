#include "CommandSchema.h"
#include "CompositionCommands.h"
#include "MusicTheory.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QSet>
#include <cstdio>
#include <limits>

int main( int argc, char ** argv )
{
	QCoreApplication application( argc, argv );
	int checks = 0, failures = 0;
	auto check = [&checks, &failures]( bool passed, const char * name )
	{
		++checks;
		if( !passed ) { ++failures; std::fprintf( stderr, "FAIL: %s\n", name ); }
	};
	using namespace CommandSchema;
	check( validate( 10, number( 10, 999, true ) ).isEmpty(), "integer minimum" );
	check( validate( 999, number( 10, 999, true ) ).isEmpty(), "integer maximum" );
	check( !validate( 9, number( 10, 999, true ) ).isEmpty(), "minimum bound" );
	check( !validate( 1000, number( 10, 999, true ) ).isEmpty(), "maximum bound" );
	check( !validate( 120.5, number( 10, 999, true ) ).isEmpty(), "fractional integer" );
	check( !validate( true, number( 10, 999, true ) ).isEmpty(), "boolean is not an integer" );
	check( !validate( "120", number( 10, 999, true ) ).isEmpty(), "string is not a number" );
	check( !validate( std::numeric_limits<double>::infinity(), number( -1e9, 1e9 ) ).isEmpty(), "infinite number" );
	check( !validate( std::numeric_limits<double>::quiet_NaN(), number( -1e9, 1e9 ) ).isEmpty(), "NaN number" );
	check( validate( true, boolean() ).isEmpty(), "boolean" );
	check( !validate( 1, boolean() ).isEmpty(), "number is not a boolean" );
	check( validate( "wav", text( { "wav", "ogg" } ) ).isEmpty(), "enum member" );
	check( !validate( "aiff", text( { "wav", "ogg" } ) ).isEmpty(), "invalid enum" );
	check( !validate( "long", text( QStringList(), 3 ) ).isEmpty(), "text length limit" );
	const QJsonObject record = object( properties( {{ "volume", number( 0, 200 ) }} ), { "volume" } );
	check( !validate( QJsonObject(), record ).isEmpty(), "required field" );
	check( validate( QJsonObject{ { "volume", 100 } }, record ).isEmpty(), "valid object" );
	check( !validate( QJsonObject{ { "volume", 100 }, { "unknown", true } }, record ).isEmpty(), "unknown property" );
	check( !validate( QJsonArray(), record ).isEmpty(), "wrong object type" );
	check( !validate( QJsonArray(), array( record, 2, 1 ) ).isEmpty(), "minimum item count" );
	check( validate( QJsonArray() << QJsonObject{ { "volume", 80 } }, array( record, 2, 1 ) ).isEmpty(), "nested array" );
	check( !validate( QJsonArray() << QJsonObject(), array( record, 2, 1 ) ).isEmpty(), "nested required field" );
	check( !validate( QJsonArray() << 1 << 2 << 3, array( number( 0, 10 ), 2 ) ).isEmpty(), "maximum item count" );
	QJsonObject either;
	either["oneOf"] = QJsonArray() << number( 0, 10 ) << boolean();
	check( validate( 4.5, either ).isEmpty() && validate( false, either ).isEmpty(), "exclusive union" );
	check( !validate( "4", either ).isEmpty(), "union mismatch" );
	either["oneOf"] = QJsonArray() << number( 0, 10 ) << number( 0, 10, true );
	check( !validate( 4, either ).isEmpty(), "ambiguous union" );

	QSet<QString> names;
	QJsonObject schemas;
	const QJsonArray tools = CompositionCommands::tools();
	check( tools.size() >= 100, "full native tool catalog" );
	for( const QJsonValue & entry : tools )
	{
		const QJsonObject tool = entry.toObject(), schema = tool["inputSchema"].toObject();
		const QString name = tool["name"].toString();
		check( !name.isEmpty() && !names.contains( name ), "unique tool name" );
		names.insert( name );
		schemas[name] = schema;
		check( schema["type"].toString() == "object" && !schema["additionalProperties"].toBool( true ), "closed tool schema" );
		check( tool["annotations"].toObject()["readOnlyHint"].isBool(), "read-only annotation" );
		check( !tool["description"].toString().isEmpty(), "tool description" );
		if( !schema["required"].toArray().isEmpty() ) { check( !validate( QJsonObject(), schema ).isEmpty(), "required tool arguments" ); }
		check( !validate( QJsonObject{ { "unsupported_argument", true } }, schema ).isEmpty(), "tool rejects unknown arguments" );
	}
	auto accepts = [&schemas]( const char * name, const QJsonObject & args )
	{
		return schemas.contains( name ) && validate( args, schemas[name].toObject() ).isEmpty();
	};
	for( const char * name : { "create_track", "update_project", "update_track", "update_clip", "update_mixer_channel", "update_effect", "transport" } )
	{
		check( names.contains( name ), "consolidated native command" );
	}
	check( accepts( "create_track", QJsonObject{ { "type", "instrument" }, { "volume", 35 }, { "container", "bb" } } ), "consolidated instrument creation" );
	check( !accepts( "create_track", QJsonObject{ { "type", "unknown" } } ), "unknown consolidated track type" );
	check( !accepts( "create_track", QJsonObject() ), "consolidated creation requires type" );
	check( accepts( "update_project", QJsonObject{ { "bpm", 126 }, { "numerator", 3 }, { "denominator", 4 }, { "master_volume", 70 }, { "master_pitch", -1 } } ), "combined project settings" );
	check( !accepts( "update_project", QJsonObject{ { "bpm", 126.5 } } ), "combined project rejects fractional BPM" );
	check( !accepts( "update_project", QJsonObject{ { "master_volume", 201 } } ), "combined project gain bounds" );
	check( accepts( "update_track", QJsonObject{ { "track_index", 0 }, { "name", "Lead" }, { "volume", 35 }, { "panning", -10 }, { "muted", false }, { "solo", true }, { "fx_channel", 1 } } ), "combined track properties" );
	check( !accepts( "update_track", QJsonObject{ { "track_index", 0 }, { "volume", -1 } } ), "combined track gain bounds" );
	check( accepts( "update_clip", QJsonObject{ { "track_index", 0 }, { "clip_index", 0 }, { "name", "Phrase" }, { "new_pos_bars", 2 }, { "length_ticks", 192 } } ), "combined clip properties" );
	check( accepts( "update_mixer_channel", QJsonObject{ { "channel_num", 1 }, { "name", "Bus" }, { "volume", 0.5 }, { "muted", false }, { "solo", true } } ), "combined mixer properties" );
	check( !accepts( "update_mixer_channel", QJsonObject{ { "channel_num", 1 }, { "volume", 3 } } ), "combined mixer gain bounds" );
	check( accepts( "update_effect", QJsonObject{ { "target_type", "mixer" }, { "target_index", 1 }, { "effect_index", 0 }, { "enabled", false }, { "wet", 0.5 } } ), "combined effect properties" );
	check( !accepts( "update_effect", QJsonObject{ { "target_type", "mixer" }, { "target_index", 1 }, { "effect_index", 0 }, { "wet", 2 } } ), "combined effect wet bounds" );
	check( accepts( "transport", QJsonObject{ { "action", "seek" }, { "position", 96 } } ), "consolidated transport seek" );
	check( !accepts( "transport", QJsonObject{ { "action", "rewind" } } ), "unknown consolidated transport action" );
	check( accepts( "load_instrument_file", QJsonObject{ { "track_index", 0 }, { "path", "C:/preset.xpf" }, { "preserve_name", false } } ), "unified file loader preserves name option" );
	check( accepts( "add_notes_batch", QJsonObject{ { "track_index", 0 }, { "notes", QJsonArray() << QJsonObject{ { "key", 60 } } }, { "include_pattern", false } } ), "native insertion can omit its full pattern response" );
	check( accepts( "set_automation_points", QJsonObject{ { "track_index", 0 }, { "mode", "upsert" }, { "points", QJsonArray() << QJsonObject{ { "pos", 48 }, { "value", 25 } } } } ), "automation point upsert mode" );
	check( !accepts( "set_automation_points", QJsonObject{ { "track_index", 0 }, { "mode", "merge" }, { "points", QJsonArray() } } ), "unknown automation mode" );
	check( !validate( QString( 32768, 'x' ), text() ).isEmpty(), "native default string limit retained" );
	check( !accepts( "update_project", QJsonObject{ { "numerator", 3 } } ), "meter fields paired" );
	check( !accepts( "update_project", QJsonObject() ), "project needs editable property" );
	check( !accepts( "create_track", QJsonObject{ { "type", "sample" }, { "volume", 30 } } ), "instrument-only creation fields" );
	check( !accepts( "transport", QJsonObject{ { "action", "play" }, { "position", 0 } } ), "seek-only position" );
	check( !accepts( "transport", QJsonObject{ { "action", "seek" } } ), "seek position required" );
	check( !accepts( "place_pattern", QJsonObject{ { "track_index", 0 }, { "pos_ticks", 0 }, { "pos_bars", 1 } } ), "exclusive placement coordinates" );
	check( !accepts( "render_project", QJsonObject{ { "path", "C:/x.wav" }, { "bit_depth", 20 } } ), "supported bit depths" );
	check( !accepts( "render_project", QJsonObject{ { "path", "C:/x.flac" }, { "format", "flac" }, { "bit_depth", 32 } } ), "FLAC bit-depth constraint" );
	check( accepts( "update_track", QJsonObject{ { "track_handle", "project/track/2" }, { "volume", 40 } } ), "handle track target" );
	check( accepts( "update_clip", QJsonObject{ { "clip_handle", "project/clip/3" }, { "name", "Phrase" } } ), "handle clip target" );
	check( !accepts( "update_track", QJsonObject{ { "track_handle", "project/track/2" } } ), "handle target still needs an edit" );
	check( !accepts( "get_track", QJsonObject{ { "track_handle", "project/track/2" }, { "track_index", 0 } } ), "unambiguous track selector" );
	check( accepts( "get_effect_chain", QJsonObject{ { "target_type", "track" }, { "track_handle", "project/track/2" } } ), "effect track handle" );
	check( !accepts( "get_effect_chain", QJsonObject{ { "target_type", "mixer" }, { "track_handle", "project/track/2" } } ), "mixer requires mixer index" );
	check( accepts( "set_notes_batch", QJsonObject{ { "clip_handle", "project/clip/3" }, { "edits", QJsonArray() << QJsonObject{ { "note_index", 1 }, { "volume", 70 } } } } ), "batch note edit handle" );
	check( !accepts( "set_notes_batch", QJsonObject{ { "track_index", 0 }, { "edits", QJsonArray() << QJsonObject{ { "note_index", 1 } } } } ), "batch note needs edit property" );
	check( !accepts( "add_notes_batch", QJsonObject{ { "track_index", 0 }, { "notes", QJsonArray() << QJsonObject{ { "key", 60 }, { "note_name", "E4" } } } } ), "exclusive note pitch fields" );
	check( accepts( "replace_notes", QJsonObject{ { "track_index", 0 }, { "notes", QJsonArray() } } ), "empty replacement clears notes" );
	check( accepts( "place_patterns_batch", QJsonObject{ { "placements", QJsonArray() << QJsonObject{ { "track_handle", "project/track/2" }, { "pos_bars", 2 }, { "name", "Copy" } } } } ), "batch pattern placement" );
	check( accepts( "list_samples", QJsonObject{ { "search", "cymbal" }, { "offset", 0 }, { "limit", 10 } } ), "sample search pagination" );

	QString error;
	check( MusicTheory::midiKey( "C4", error ) == 60 && error.isEmpty(), "C4 MIDI mapping" );
	check( MusicTheory::midiKey( "Bb2", error ) == 46 && error.isEmpty(), "flat note name" );
	check( MusicTheory::midiKey( "B#3", error ) == 60 && error.isEmpty(), "enharmonic note" );
	check( MusicTheory::midiKey( "C-1", error ) == 0 && error.isEmpty(), "minimum MIDI note" );
	check( MusicTheory::midiKey( "G9", error ) == 127 && error.isEmpty(), "maximum MIDI note" );
	for( const QString & invalid : QStringList{ "H4", "C", "C##4", "Cb-1", "G#9", "C100000000000" } )
	{
		error.clear();
		check( MusicTheory::midiKey( invalid, error ) == -1 && !error.isEmpty(), "invalid note name" );
	}
	for( int key = 0; key <= 127; ++key )
	{
		error.clear();
		check( MusicTheory::midiKey( MusicTheory::noteName( key ), error ) == key && error.isEmpty(), "MIDI name round-trip" );
	}
	check( MusicTheory::noteName( -1 ).isEmpty() && MusicTheory::noteName( 128 ).isEmpty(), "invalid MIDI key" );
	check( MusicTheory::scales().value( "major" ) == QVector<int>{ 0, 2, 4, 5, 7, 9, 11 }, "major scale" );
	std::printf( "%s: %d native schema/music-theory checks; %d failures\n", failures ? "FAIL" : "PASS", checks, failures );
	return failures ? 1 : 0;
}
