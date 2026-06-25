#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

void RegisterDpSassStabilityQueryFunction(ExtensionLoader &loader);

} // namespace duckdb
