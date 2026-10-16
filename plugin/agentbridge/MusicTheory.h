#ifndef AGENTBRIDGE_MUSIC_THEORY_H
#define AGENTBRIDGE_MUSIC_THEORY_H

#include <QtCore/QMap>
#include <QtCore/QRegExp>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace MusicTheory
{

inline int midiKey( const QString & name, QString & error )
{
	QRegExp expression( "^([A-Ga-g])([#b]?)(-?[0-9]+)$" );
	if( !expression.exactMatch( name.trimmed() ) ) { error = "note name must be like C4, F#5, or Bb2"; return -1; }
	const QString letters = "CDEFGAB";
	const int semitones[] = { 0, 2, 4, 5, 7, 9, 11 };
	bool valid = false;
	const int octave = expression.cap( 3 ).toInt( &valid );
	if( !valid || octave < -1 || octave > 9 ) { error = "note octave is outside the MIDI range"; return -1; }
	const int accidental = expression.cap( 2 ) == "#" ? 1 : expression.cap( 2 ) == "b" ? -1 : 0;
	const int key = ( octave + 1 ) * 12 + semitones[letters.indexOf( expression.cap( 1 ).toUpper() )] + accidental;
	if( key < 0 || key > 127 ) { error = "note is outside MIDI keys 0..127"; return -1; }
	return key;
}

inline QString noteName( int key )
{
	if( key < 0 || key > 127 ) { return QString(); }
	const QStringList names = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	return names[key % 12] + QString::number( key / 12 - 1 );
}

inline const QMap<QString, QVector<int> > & scales()
{
	static const QMap<QString, QVector<int> > values = {
		{ "major", { 0, 2, 4, 5, 7, 9, 11 } }, { "minor", { 0, 2, 3, 5, 7, 8, 10 } },
		{ "harmonic_minor", { 0, 2, 3, 5, 7, 8, 11 } }, { "melodic_minor", { 0, 2, 3, 5, 7, 9, 11 } },
		{ "pentatonic_major", { 0, 2, 4, 7, 9 } }, { "pentatonic_minor", { 0, 3, 5, 7, 10 } },
		{ "blues", { 0, 3, 5, 6, 7, 10 } }, { "dorian", { 0, 2, 3, 5, 7, 9, 10 } },
		{ "phrygian", { 0, 1, 3, 5, 7, 8, 10 } }, { "lydian", { 0, 2, 4, 6, 7, 9, 11 } },
		{ "mixolydian", { 0, 2, 4, 5, 7, 9, 10 } }, { "locrian", { 0, 1, 3, 5, 6, 8, 10 } },
		{ "chromatic", { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } }
	};
	return values;
}

}

#endif
