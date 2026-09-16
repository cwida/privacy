#pragma once

namespace duckdb {

class ExtensionLoader;

void RegisterPrivacyOwnershipBoundsDebugFunction(ExtensionLoader &loader);

} // namespace duckdb
