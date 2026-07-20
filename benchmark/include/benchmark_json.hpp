#pragma once

#include "duckdb.hpp"

#include <cctype>
#include <map>
#include <stdexcept>
#include <utility>

namespace duckdb {

enum class JsonType : uint8_t { NIL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

struct JsonValue {
	JsonType type = JsonType::NIL;
	bool boolean = false;
	double number = 0;
	string str;
	vector<JsonValue> array;
	std::map<string, JsonValue> object;

	bool Has(const string &key) const {
		return type == JsonType::OBJECT && object.find(key) != object.end();
	}

	const JsonValue &Get(const string &key) const {
		static JsonValue nil;
		if (type != JsonType::OBJECT) {
			return nil;
		}
		auto entry = object.find(key);
		return entry == object.end() ? nil : entry->second;
	}
};

class JsonParser {
public:
	explicit JsonParser(string input_p) : input(std::move(input_p)) {
	}

	JsonValue Parse() {
		SkipWhitespace();
		auto value = ParseValue();
		SkipWhitespace();
		if (position != input.size()) {
			throw std::runtime_error("unexpected trailing JSON at byte " + std::to_string(position));
		}
		return value;
	}

private:
	string input;
	idx_t position = 0;

	void SkipWhitespace() {
		while (position < input.size() && std::isspace(static_cast<unsigned char>(input[position]))) {
			position++;
		}
	}

	bool Consume(char token) {
		SkipWhitespace();
		if (position < input.size() && input[position] == token) {
			position++;
			return true;
		}
		return false;
	}

	void Expect(char token) {
		if (!Consume(token)) {
			throw std::runtime_error(string("expected '") + token + "' at byte " + std::to_string(position));
		}
	}

	JsonValue ParseValue() {
		SkipWhitespace();
		if (position >= input.size()) {
			throw std::runtime_error("unexpected end of JSON");
		}
		char token = input[position];
		if (token == '{') {
			return ParseObject();
		}
		if (token == '[') {
			return ParseArray();
		}
		if (token == '"') {
			JsonValue value;
			value.type = JsonType::STRING;
			value.str = ParseString();
			return value;
		}
		if (token == '-' || std::isdigit(static_cast<unsigned char>(token))) {
			return ParseNumber();
		}
		if (input.compare(position, 4, "true") == 0) {
			position += 4;
			JsonValue value;
			value.type = JsonType::BOOL;
			value.boolean = true;
			return value;
		}
		if (input.compare(position, 5, "false") == 0) {
			position += 5;
			JsonValue value;
			value.type = JsonType::BOOL;
			return value;
		}
		if (input.compare(position, 4, "null") == 0) {
			position += 4;
			return JsonValue();
		}
		throw std::runtime_error("unexpected JSON token at byte " + std::to_string(position));
	}

	JsonValue ParseObject() {
		JsonValue value;
		value.type = JsonType::OBJECT;
		Expect('{');
		if (Consume('}')) {
			return value;
		}
		while (true) {
			SkipWhitespace();
			if (position >= input.size() || input[position] != '"') {
				throw std::runtime_error("expected object key at byte " + std::to_string(position));
			}
			string key = ParseString();
			Expect(':');
			value.object[key] = ParseValue();
			if (Consume('}')) {
				break;
			}
			Expect(',');
		}
		return value;
	}

	JsonValue ParseArray() {
		JsonValue value;
		value.type = JsonType::ARRAY;
		Expect('[');
		if (Consume(']')) {
			return value;
		}
		while (true) {
			value.array.push_back(ParseValue());
			if (Consume(']')) {
				break;
			}
			Expect(',');
		}
		return value;
	}

	string ParseString() {
		Expect('"');
		string result;
		while (position < input.size()) {
			char token = input[position++];
			if (token == '"') {
				return result;
			}
			if (token != '\\') {
				result += token;
				continue;
			}
			if (position >= input.size()) {
				throw std::runtime_error("unterminated JSON escape");
			}
			char escaped = input[position++];
			switch (escaped) {
			case '"':
			case '\\':
			case '/':
				result += escaped;
				break;
			case 'b':
				result += '\b';
				break;
			case 'f':
				result += '\f';
				break;
			case 'n':
				result += '\n';
				break;
			case 'r':
				result += '\r';
				break;
			case 't':
				result += '\t';
				break;
			default:
				throw std::runtime_error("unsupported JSON escape");
			}
		}
		throw std::runtime_error("unterminated JSON string");
	}

	JsonValue ParseNumber() {
		idx_t start = position;
		if (input[position] == '-') {
			position++;
		}
		while (position < input.size() && std::isdigit(static_cast<unsigned char>(input[position]))) {
			position++;
		}
		if (position < input.size() && input[position] == '.') {
			position++;
			while (position < input.size() && std::isdigit(static_cast<unsigned char>(input[position]))) {
				position++;
			}
		}
		if (position < input.size() && (input[position] == 'e' || input[position] == 'E')) {
			position++;
			if (position < input.size() && (input[position] == '+' || input[position] == '-')) {
				position++;
			}
			while (position < input.size() && std::isdigit(static_cast<unsigned char>(input[position]))) {
				position++;
			}
		}
		JsonValue value;
		value.type = JsonType::NUMBER;
		value.number = std::stod(input.substr(start, position - start));
		return value;
	}
};

static string JsonString(const JsonValue &object, const string &key, const string &default_value = "") {
	const auto &value = object.Get(key);
	return value.type == JsonType::STRING ? value.str : default_value;
}

static double JsonNumber(const JsonValue &object, const string &key, double default_value) {
	const auto &value = object.Get(key);
	return value.type == JsonType::NUMBER ? value.number : default_value;
}

static idx_t JsonIdx(const JsonValue &object, const string &key, idx_t default_value) {
	const auto &value = object.Get(key);
	return value.type == JsonType::NUMBER ? static_cast<idx_t>(value.number) : default_value;
}

static bool JsonBool(const JsonValue &object, const string &key, bool default_value) {
	const auto &value = object.Get(key);
	return value.type == JsonType::BOOL ? value.boolean : default_value;
}

static string FindConfigPathArgument(int argc, char **argv) {
	string config_path;
	for (int i = 1; i < argc; i++) {
		string arg = argv[i];
		if (arg == "--config") {
			if (i + 1 >= argc) {
				throw std::runtime_error("--config requires a value");
			}
			string value = argv[++i];
			if (value.empty() || value.rfind("--", 0) == 0) {
				throw std::runtime_error("--config requires a non-empty path");
			}
			config_path = std::move(value);
		} else if (arg.rfind("--config=", 0) == 0) {
			string value = arg.substr(9);
			if (value.empty()) {
				throw std::runtime_error("--config requires a non-empty path");
			}
			config_path = std::move(value);
		}
	}
	return config_path;
}

static vector<string> JsonStringList(const JsonValue &object, const string &key, const vector<string> &default_value) {
	const auto &value = object.Get(key);
	if (value.type == JsonType::STRING) {
		return {value.str};
	}
	if (value.type != JsonType::ARRAY) {
		return default_value;
	}
	vector<string> result;
	for (auto &entry : value.array) {
		if (entry.type != JsonType::STRING) {
			throw std::runtime_error(key + " must contain only strings");
		}
		result.push_back(entry.str);
	}
	return result;
}

static vector<double> JsonNumberList(const JsonValue &object, const string &array_key, const string &scalar_key,
                                     const vector<double> &default_value) {
	const auto &scalar = object.Get(scalar_key);
	if (scalar.type == JsonType::NUMBER) {
		return {scalar.number};
	}
	const auto &array = object.Get(array_key);
	if (array.type != JsonType::ARRAY) {
		return default_value;
	}
	vector<double> result;
	for (auto &entry : array.array) {
		if (entry.type != JsonType::NUMBER) {
			throw std::runtime_error(array_key + " must contain only numbers");
		}
		result.push_back(entry.number);
	}
	return result;
}

static vector<int> JsonIntList(const JsonValue &object, const string &array_key, const string &scalar_key,
                               const vector<int> &default_value) {
	const auto &scalar = object.Get(scalar_key);
	if (scalar.type == JsonType::NUMBER) {
		return {static_cast<int>(scalar.number)};
	}
	const auto &array = object.Get(array_key);
	if (array.type != JsonType::ARRAY) {
		return default_value;
	}
	vector<int> result;
	for (auto &entry : array.array) {
		if (entry.type != JsonType::NUMBER || std::floor(entry.number) != entry.number) {
			throw std::runtime_error(array_key + " must contain only integers");
		}
		result.push_back(static_cast<int>(entry.number));
	}
	return result;
}

static vector<bool> JsonBoolList(const JsonValue &object, const string &array_key, const string &scalar_key,
                                 const vector<bool> &default_value) {
	const auto &scalar = object.Get(scalar_key);
	if (scalar.type == JsonType::BOOL) {
		return {scalar.boolean};
	}
	const auto &array = object.Get(array_key);
	if (array.type != JsonType::ARRAY) {
		return default_value;
	}
	vector<bool> result;
	for (auto &entry : array.array) {
		if (entry.type != JsonType::BOOL) {
			throw std::runtime_error(array_key + " must contain only booleans");
		}
		result.push_back(entry.boolean);
	}
	return result;
}

} // namespace duckdb
