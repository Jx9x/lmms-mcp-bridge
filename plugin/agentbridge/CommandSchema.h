#ifndef AGENTBRIDGE_COMMAND_SCHEMA_H
#define AGENTBRIDGE_COMMAND_SCHEMA_H

#include <cmath>
#include <initializer_list>
#include <utility>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>

namespace CommandSchema
{

inline QJsonObject number( double minimum, double maximum, bool integer = false )
{
	QJsonObject o;
	o["type"] = integer ? "integer" : "number";
	o["minimum"] = minimum;
	o["maximum"] = maximum;
	return o;
}

inline QJsonObject text( const QStringList & choices = QStringList(), int maximum = 32767 )
{
	QJsonObject o;
	o["type"] = "string";
	o["maxLength"] = maximum;
	if( !choices.isEmpty() )
	{
		QJsonArray a;
		for( const QString & choice : choices ) { a.append( choice ); }
		o["enum"] = a;
	}
	return o;
}

inline QJsonObject boolean()
{
	QJsonObject o;
	o["type"] = "boolean";
	return o;
}

inline QJsonObject properties( std::initializer_list<std::pair<const char *, QJsonObject> > fields )
{
	QJsonObject o;
	for( const auto & field : fields ) { o[field.first] = field.second; }
	return o;
}

inline QJsonObject object( const QJsonObject & properties = QJsonObject(), const QStringList & required = QStringList() )
{
	QJsonObject o;
	o["type"] = "object";
	o["properties"] = properties;
	o["additionalProperties"] = false;
	QJsonArray a;
	for( const QString & name : required ) { a.append( name ); }
	o["required"] = a;
	return o;
}

inline QJsonObject array( const QJsonObject & items, int maximum = 512, int minimum = 0 )
{
	QJsonObject o;
	o["type"] = "array";
	o["items"] = items;
	o["minItems"] = minimum;
	o["maxItems"] = maximum;
	return o;
}

inline QJsonObject tool( const QString & name, const QString & description, const QJsonObject & schema,
		bool readOnly = false, bool destructive = false )
{
	QJsonObject o;
	o["name"] = name;
	o["description"] = description;
	o["inputSchema"] = schema;
	QJsonObject annotations;
	annotations["readOnlyHint"] = readOnly;
	annotations["destructiveHint"] = destructive;
	annotations["openWorldHint"] = false;
	o["annotations"] = annotations;
	return o;
}

inline QString validate( const QJsonValue & value, const QJsonObject & schema, const QString & path = "args" )
{
	if( schema.contains( "const" ) && schema["const"] != value ) { return path + " has an unexpected value"; }
	if( schema.contains( "not" ) && validate( value, schema["not"].toObject(), path ).isEmpty() ) { return path + " contains mutually exclusive or forbidden arguments"; }
	for( const QJsonValue & part : schema["allOf"].toArray() )
	{
		const QString error = validate( value, part.toObject(), path );
		if( !error.isEmpty() ) { return error; }
	}
	if( schema.contains( "anyOf" ) )
	{
		bool matched = false;
		for( const QJsonValue & part : schema["anyOf"].toArray() ) { matched |= validate( value, part.toObject(), path ).isEmpty(); }
		if( !matched ) { return path + " is missing a required argument combination"; }
	}
	if( schema.contains( "if" ) )
	{
		const QString branch = validate( value, schema["if"].toObject(), path ).isEmpty() ? "then" : "else";
		if( schema.contains( branch ) )
		{
			const QString error = validate( value, schema[branch].toObject(), path );
			if( !error.isEmpty() ) { return error; }
		}
	}
	if( schema.contains( "oneOf" ) )
	{
		int matches = 0;
		for( const QJsonValue & option : schema["oneOf"].toArray() )
		{
			matches += validate( value, option.toObject(), path ).isEmpty() ? 1 : 0;
		}
		if( matches != 1 ) { return path + " does not match an allowed value/type"; }
	}
	const QString type = schema["type"].toString();
	if( type == "object" || ( type.isEmpty() && value.isObject() ) )
	{
		if( !value.isObject() ) { return path + " must be an object"; }
		const QJsonObject o = value.toObject();
		const QJsonObject props = schema["properties"].toObject();
		for( const QJsonValue & required : schema["required"].toArray() )
		{
			if( !o.contains( required.toString() ) ) { return path + "." + required.toString() + " is required"; }
		}
		const QJsonObject dependencies = schema["dependencies"].toObject();
		for( auto it = dependencies.begin(); it != dependencies.end(); ++it )
		{
			if( !o.contains( it.key() ) ) { continue; }
			for( const QJsonValue & field : it.value().toArray() ) { if( !o.contains( field.toString() ) ) { return path + "." + field.toString() + " is required with " + it.key(); } }
		}
		for( auto it = o.begin(); it != o.end(); ++it )
		{
			if( !props.contains( it.key() ) )
			{
				if( !schema["additionalProperties"].toBool( true ) ) { return path + "." + it.key() + " is not a supported argument"; }
				continue;
			}
			const QString error = validate( it.value(), props[it.key()].toObject(), path + "." + it.key() );
			if( !error.isEmpty() ) { return error; }
		}
	}
	else if( type == "array" )
	{
		if( !value.isArray() ) { return path + " must be an array"; }
		const QJsonArray a = value.toArray();
		if( a.size() < schema["minItems"].toInt() || a.size() > schema["maxItems"].toInt( 512 ) )
		{
			return path + " has an invalid item count";
		}
		for( int i = 0; i < a.size(); ++i )
		{
			const QString error = validate( a[i], schema["items"].toObject(), path + QString( "[%1]" ).arg( i ) );
			if( !error.isEmpty() ) { return error; }
		}
	}
	else if( type == "integer" || type == "number" )
	{
		const double v = value.toDouble();
		if( !value.isDouble() || !std::isfinite( v ) || ( type == "integer" && std::floor( v ) != v ) )
		{
			return path + " must be a finite " + type;
		}
		if( ( schema.contains( "minimum" ) && v < schema["minimum"].toDouble() ) ||
			( schema.contains( "maximum" ) && v > schema["maximum"].toDouble() ) )
		{
			return path + QString( " must be in %1..%2" ).arg( schema["minimum"].toDouble() ).arg( schema["maximum"].toDouble() );
		}
	}
	else if( type == "string" )
	{
		if( !value.isString() ) { return path + " must be a string"; }
		if( value.toString().size() > schema["maxLength"].toInt( 32767 ) ) { return path + " is too long"; }
	}
	else if( type == "boolean" && !value.isBool() ) { return path + " must be a boolean"; }
	if( schema.contains( "enum" ) && !schema["enum"].toArray().contains( value ) ) { return path + " is not an allowed value"; }
	return QString();
}

}

#endif
