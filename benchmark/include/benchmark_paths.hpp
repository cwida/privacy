#pragma once

#include "duckdb.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>

namespace duckdb {

static void EnsureDirectory(const string &path) {
	if (path.empty()) {
		return;
	}
	struct stat info;
	if (stat(path.c_str(), &info) == 0) {
		if (S_ISDIR(info.st_mode)) {
			return;
		}
		throw std::runtime_error("output parent is not a directory: " + path);
	}
	auto separator = path.find_last_of('/');
	if (separator != string::npos) {
		EnsureDirectory(path.substr(0, separator));
	}
	if (mkdir(path.c_str(), 0777) != 0) {
		if (errno != EEXIST || stat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
			throw std::runtime_error("could not create output directory " + path + ": " + std::strerror(errno));
		}
	}
}

static void EnsureOutputParentDirectory(const string &output_path) {
	if (output_path.empty()) {
		throw std::runtime_error("output path must not be empty");
	}
	auto separator = output_path.find_last_of('/');
	if (separator == string::npos) {
		return;
	}
	EnsureDirectory(output_path.substr(0, separator));
}

} // namespace duckdb
