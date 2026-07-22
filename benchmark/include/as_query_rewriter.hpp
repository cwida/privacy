#ifndef AS_QUERY_REWRITER_HPP
#define AS_QUERY_REWRITER_HPP

#include "duckdb.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace duckdb {
namespace benchmark {

struct AsQueryRewriteOptions {
	const vector<string> &function_names;
	const vector<string> &unsupported_function_names;
	bool strip_line_comments;
	bool rewrite_nested_arguments;
	string non_recursive_function;
	bool reject_priv_select;
};

static inline string TrimSqlText(const string &value) {
	auto begin = std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); });
	auto end = std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base();
	if (begin >= end) {
		return string();
	}
	return string(begin, end);
}

static inline bool IsIdentifierChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static inline idx_t FindMatchingParen(const string &sql, idx_t open_pos) {
	int depth = 0;
	bool in_single_quote = false;
	bool in_double_quote = false;
	for (idx_t i = open_pos; i < sql.size(); i++) {
		char c = sql[i];
		if (in_single_quote) {
			if (c == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {
				i++;
			} else if (c == '\'') {
				in_single_quote = false;
			}
			continue;
		}
		if (in_double_quote) {
			if (c == '"' && i + 1 < sql.size() && sql[i + 1] == '"') {
				i++;
			} else if (c == '"') {
				in_double_quote = false;
			}
			continue;
		}
		if (c == '\'') {
			in_single_quote = true;
		} else if (c == '"') {
			in_double_quote = true;
		} else if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
			if (depth == 0) {
				return i;
			}
		}
	}
	return string::npos;
}

static inline vector<string> SplitTopLevelArgs(const string &args) {
	vector<string> result;
	idx_t start = 0;
	int depth = 0;
	bool in_single_quote = false;
	bool in_double_quote = false;
	for (idx_t i = 0; i < args.size(); i++) {
		char c = args[i];
		if (in_single_quote) {
			if (c == '\'' && i + 1 < args.size() && args[i + 1] == '\'') {
				i++;
			} else if (c == '\'') {
				in_single_quote = false;
			}
			continue;
		}
		if (in_double_quote) {
			if (c == '"' && i + 1 < args.size() && args[i + 1] == '"') {
				i++;
			} else if (c == '"') {
				in_double_quote = false;
			}
			continue;
		}
		if (c == '\'') {
			in_single_quote = true;
		} else if (c == '"') {
			in_double_quote = true;
		} else if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
		} else if (c == ',' && depth == 0) {
			result.push_back(TrimSqlText(args.substr(start, i - start)));
			start = i + 1;
		}
	}
	result.push_back(TrimSqlText(args.substr(start)));
	return result;
}

static inline bool StartsWithFunctionCall(const string &sql, idx_t pos, const string &name, idx_t &open_pos) {
	if (pos > 0 && IsIdentifierChar(sql[pos - 1])) {
		return false;
	}
	if (sql.compare(pos, name.size(), name) != 0) {
		return false;
	}
	idx_t next = pos + name.size();
	if (next < sql.size() && IsIdentifierChar(sql[next])) {
		return false;
	}
	while (next < sql.size() && std::isspace(static_cast<unsigned char>(sql[next]))) {
		next++;
	}
	if (next >= sql.size() || sql[next] != '(') {
		return false;
	}
	open_pos = next;
	return true;
}

static inline string StripFunctionCalls(const string &sql, const string &name) {
	string result;
	idx_t pos = 0;
	while (pos < sql.size()) {
		idx_t open_pos;
		if (StartsWithFunctionCall(sql, pos, name, open_pos)) {
			idx_t close_pos = FindMatchingParen(sql, open_pos);
			if (close_pos == string::npos) {
				throw std::runtime_error("could not find closing parenthesis for " + name);
			}
			result += StripFunctionCalls(sql.substr(open_pos + 1, close_pos - open_pos - 1), name);
			pos = close_pos + 1;
			continue;
		}
		result.push_back(sql[pos++]);
	}
	return result;
}

static inline string StripSqlLineComments(const string &sql) {
	string result;
	bool in_single_quote = false;
	for (idx_t i = 0; i < sql.size(); i++) {
		char c = sql[i];
		if (c == '\'' && (i + 1 >= sql.size() || sql[i + 1] != '\'')) {
			in_single_quote = !in_single_quote;
			result.push_back(c);
			continue;
		}
		if (!in_single_quote && c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
			while (i < sql.size() && sql[i] != '\n') {
				i++;
			}
			if (i < sql.size()) {
				result.push_back(sql[i]);
			}
			continue;
		}
		result.push_back(c);
		if (c == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {
			result.push_back(sql[++i]);
		}
	}
	return result;
}

static inline bool ParseEntireFunctionCall(const string &expression, const string &name, vector<string> &args) {
	string trimmed = TrimSqlText(expression);
	idx_t open_pos;
	if (!StartsWithFunctionCall(trimmed, 0, name, open_pos)) {
		return false;
	}
	idx_t close_pos = FindMatchingParen(trimmed, open_pos);
	if (close_pos == string::npos) {
		return false;
	}
	for (idx_t i = close_pos + 1; i < trimmed.size(); i++) {
		if (!std::isspace(static_cast<unsigned char>(trimmed[i]))) {
			return false;
		}
	}
	args = SplitTopLevelArgs(trimmed.substr(open_pos + 1, close_pos - open_pos - 1));
	return true;
}

static inline bool ContainsFunctionCall(const string &sql, const vector<string> &names) {
	for (idx_t pos = 0; pos < sql.size(); pos++) {
		for (auto &name : names) {
			idx_t open_pos;
			if (StartsWithFunctionCall(sql, pos, name, open_pos)) {
				return true;
			}
		}
	}
	return false;
}

template <class REWRITER>
static string RewriteAsQuery(const string &sql, int sample_count, const AsQueryRewriteOptions &options,
                             const REWRITER &rewrite_function) {
	string input = options.strip_line_comments ? StripSqlLineComments(sql) : sql;
	string raw_hash_sql = StripFunctionCalls(input, "priv_hash");
	string result;
	idx_t pos = 0;
	while (pos < raw_hash_sql.size()) {
		bool rewritten = false;
		for (auto &name : options.function_names) {
			idx_t open_pos;
			if (!StartsWithFunctionCall(raw_hash_sql, pos, name, open_pos)) {
				continue;
			}
			idx_t close_pos = FindMatchingParen(raw_hash_sql, open_pos);
			if (close_pos == string::npos) {
				throw std::runtime_error("could not find closing parenthesis for " + name);
			}
			auto args = SplitTopLevelArgs(raw_hash_sql.substr(open_pos + 1, close_pos - open_pos - 1));
			if (options.rewrite_nested_arguments && name != options.non_recursive_function) {
				for (auto &arg : args) {
					arg = RewriteAsQuery(arg, sample_count, options, rewrite_function);
				}
			}
			result += rewrite_function(name, args, sample_count);
			pos = close_pos + 1;
			rewritten = true;
			break;
		}
		if (!rewritten) {
			result.push_back(raw_hash_sql[pos++]);
		}
	}
	if (ContainsFunctionCall(result, options.unsupported_function_names) ||
	    (options.reject_priv_select && result.find("priv_select_") != string::npos)) {
		throw std::runtime_error("variable-m AS rewrite does not support remaining 64-world list expressions");
	}
	return result;
}

} // namespace benchmark
} // namespace duckdb

#endif
