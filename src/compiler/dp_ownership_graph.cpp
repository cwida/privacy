#include "compiler/dp_ownership_graph.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_result.hpp"
#include "metadata/privacy_metadata_manager.hpp"
#include "parser/privacy_parser.hpp"
#include "privacy_debug.hpp"

#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

namespace {

static const string FUNCTION_NAME = "privacy_ownership_bounds_debug";

struct OwnershipEdge {
	idx_t child;
	idx_t parent;
	vector<string> local_columns;
	vector<string> referenced_columns;
	uint64_t observed_max_frequency = 0;
};

struct OwnershipNode {
	string table_name;
	vector<string> key_columns;
	vector<idx_t> outgoing_edges;
	bool is_pu;
};

struct OwnershipBounds {
	uint64_t max_owned_rows;
	uint64_t max_owners_per_row;
};

struct OwnershipGraph {
	// Edges point from child records toward the parent records used to find owners.
	// Parallel edges remain distinct, such as buyer_id and seller_id to person.
	vector<OwnershipNode> nodes;
	vector<OwnershipEdge> edges;
	std::unordered_map<string, idx_t> node_by_name;
	idx_t fact_node;
	idx_t pu_node;
};

static string NormalizeTableName(const string &table_name) {
	auto dot = table_name.rfind('.');
	return StringUtil::Lower(dot == string::npos ? table_name : table_name.substr(dot + 1));
}

static string QuoteIdentifier(const string &identifier) {
	string result = "\"";
	for (auto character : identifier) {
		result += character == '"' ? "\"\"" : string(1, character);
	}
	result += "\"";
	return result;
}

static string JoinQualifiedColumns(const string &alias, const vector<string> &columns, const string &separator) {
	vector<string> qualified;
	qualified.reserve(columns.size());
	for (const auto &column : columns) {
		qualified.push_back(alias + "." + QuoteIdentifier(column));
	}
	return StringUtil::Join(qualified, separator);
}

static string JoinAliasedColumns(const string &alias, const vector<string> &columns, const string &output_prefix) {
	vector<string> expressions;
	expressions.reserve(columns.size());
	for (idx_t i = 0; i < columns.size(); i++) {
		expressions.push_back(alias + "." + QuoteIdentifier(columns[i]) + " AS " +
		                      QuoteIdentifier(output_prefix + std::to_string(i)));
	}
	return StringUtil::Join(expressions, ", ");
}

static uint64_t CheckedAdd(uint64_t left, uint64_t right, const string &description) {
	if (right > std::numeric_limits<uint64_t>::max() - left) {
		throw InvalidInputException(FUNCTION_NAME + ": overflow while computing " + description);
	}
	return left + right;
}

static uint64_t CheckedMultiply(uint64_t left, uint64_t right, const string &description) {
	if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
		throw InvalidInputException(FUNCTION_NAME + ": overflow while computing " + description);
	}
	return left * right;
}

static void RunSetting(Connection &connection, const string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		throw InternalException(FUNCTION_NAME + ": failed to configure diagnostic connection: " + result->GetError());
	}
}

static Value ExecuteScalar(Connection &connection, const string &sql, const string &description) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		throw InvalidInputException(FUNCTION_NAME + ": failed to compute " + description + ": " + result->GetError());
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() != 1 || result->ColumnCount() != 1) {
		throw InternalException(FUNCTION_NAME + ": diagnostic query for " + description + " did not return one scalar");
	}
	return chunk->GetValue(0, 0);
}

static bool ExecuteExists(Connection &connection, const string &query, const string &description) {
	return ExecuteScalar(connection, "SELECT EXISTS(" + query + ")", description).GetValue<bool>();
}

static uint64_t ExecuteCount(Connection &connection, const string &query, const string &description) {
	auto value = ExecuteScalar(connection, query, description).GetValue<int64_t>();
	if (value < 0) {
		throw InternalException(FUNCTION_NAME + ": negative result while computing " + description);
	}
	return static_cast<uint64_t>(value);
}

class OwnershipGraphBuilder {
public:
	explicit OwnershipGraphBuilder(string fact_table_p) : fact_table(std::move(fact_table_p)) {
	}

	OwnershipGraph Build() {
		Visit(fact_table);
		if (pu_tables.empty()) {
			throw InvalidInputException(FUNCTION_NAME + ": table '" + fact_table +
			                            "' has no PRIVACY_LINK path to a privacy unit");
		}
		if (pu_tables.size() != 1) {
			vector<string> names(pu_tables.begin(), pu_tables.end());
			std::sort(names.begin(), names.end());
			throw InvalidInputException(FUNCTION_NAME + ": table '" + fact_table +
			                            "' reaches multiple privacy units: " + StringUtil::Join(names, ", "));
		}
		graph.fact_node = graph.node_by_name.at(fact_table);
		graph.pu_node = graph.node_by_name.at(*pu_tables.begin());
		if (graph.fact_node == graph.pu_node) {
			throw InvalidInputException(FUNCTION_NAME + ": table '" + fact_table +
			                            "' is already a privacy unit and has no ownership edges to analyze");
		}
		if (graph.nodes[graph.fact_node].key_columns.empty()) {
			throw InvalidInputException(FUNCTION_NAME + ": fact table '" + fact_table +
			                            "' requires a PRIVACY_KEY to identify rows across ownership paths");
		}
		if (graph.nodes[graph.pu_node].key_columns.empty()) {
			throw InvalidInputException(FUNCTION_NAME + ": privacy unit table '" + *pu_tables.begin() +
			                            "' requires a PRIVACY_KEY");
		}
		return std::move(graph);
	}

private:
	void Visit(const string &table_name) {
		auto state_it = visit_state.find(table_name);
		if (state_it != visit_state.end()) {
			if (state_it->second == 1) {
				throw InvalidInputException(FUNCTION_NAME + ": cycle detected at table '" + table_name + "'");
			}
			return;
		}
		visit_state[table_name] = 1;

		auto *metadata_ptr = PrivacyMetadataManager::Get().GetTableMetadata(table_name);
		if (!metadata_ptr) {
			throw InvalidInputException(FUNCTION_NAME + ": table '" + table_name + "' has no privacy metadata");
		}
		auto metadata = *metadata_ptr;
		idx_t node_index = EnsureNode(table_name, metadata);
		if (metadata.is_privacy_unit) {
			pu_tables.insert(table_name);
			visit_state[table_name] = 2;
			return;
		}
		if (metadata.links.empty()) {
			throw InvalidInputException(FUNCTION_NAME + ": ownership path ends at non-PU table '" + table_name + "'");
		}

		for (const auto &link : metadata.links) {
			if (link.local_columns.empty() || link.local_columns.size() != link.referenced_columns.size()) {
				throw InvalidInputException(FUNCTION_NAME + ": invalid PRIVACY_LINK column mapping on table '" +
				                            table_name + "'");
			}
			string parent_name = NormalizeTableName(link.referenced_table);
			Visit(parent_name);
			OwnershipEdge edge;
			edge.child = node_index;
			edge.parent = graph.node_by_name.at(parent_name);
			edge.local_columns = link.local_columns;
			edge.referenced_columns = link.referenced_columns;
			graph.nodes[node_index].outgoing_edges.push_back(graph.edges.size());
			graph.edges.push_back(std::move(edge));
		}
		visit_state[table_name] = 2;
	}

	idx_t EnsureNode(const string &table_name, const PrivacyTableMetadata &metadata) {
		auto existing = graph.node_by_name.find(table_name);
		if (existing != graph.node_by_name.end()) {
			return existing->second;
		}
		idx_t index = graph.nodes.size();
		graph.node_by_name.emplace(table_name, index);
		graph.nodes.push_back({table_name, metadata.primary_key_columns, {}, metadata.is_privacy_unit});
		return index;
	}

private:
	string fact_table;
	OwnershipGraph graph;
	std::unordered_map<string, uint8_t> visit_state;
	std::unordered_set<string> pu_tables;
};

static void ValidateKey(Connection &connection, const string &table_name, const vector<string> &columns,
                        const string &description) {
	string table = QuoteIdentifier(table_name);
	string null_predicate;
	for (idx_t i = 0; i < columns.size(); i++) {
		if (i > 0) {
			null_predicate += " OR ";
		}
		null_predicate += QuoteIdentifier(columns[i]) + " IS NULL";
	}
	if (ExecuteExists(connection, "SELECT 1 FROM " + table + " WHERE " + null_predicate + " LIMIT 1",
	                  description + " NULL check")) {
		throw InvalidInputException(FUNCTION_NAME + ": " + description + " on table '" + table_name +
		                            "' contains NULL values");
	}
	string columns_sql = JoinQualifiedColumns("key_table", columns, ", ");
	if (ExecuteExists(connection,
	                  "SELECT 1 FROM " + table + " AS key_table GROUP BY " + columns_sql +
	                      " HAVING COUNT(*) > 1 LIMIT 1",
	                  description + " uniqueness check")) {
		throw InvalidInputException(FUNCTION_NAME + ": " + description + " on table '" + table_name +
		                            "' is not unique");
	}
}

static uint64_t ValidateLinkAndGetMaxFrequency(Connection &connection, const OwnershipGraph &graph,
                                               const OwnershipEdge &edge) {
	const auto &child = graph.nodes[edge.child];
	const auto &parent = graph.nodes[edge.parent];
	string child_table = QuoteIdentifier(child.table_name);
	string parent_table = QuoteIdentifier(parent.table_name);

	string null_predicate;
	for (idx_t i = 0; i < edge.local_columns.size(); i++) {
		if (i > 0) {
			null_predicate += " OR ";
		}
		null_predicate += "child." + QuoteIdentifier(edge.local_columns[i]) + " IS NULL";
	}
	if (ExecuteExists(connection, "SELECT 1 FROM " + child_table + " AS child WHERE " + null_predicate + " LIMIT 1",
	                  "NULL-link check")) {
		throw InvalidInputException(FUNCTION_NAME + ": PRIVACY_LINK from '" + child.table_name + "' to '" +
		                            parent.table_name + "' contains NULL values");
	}

	vector<string> equalities;
	equalities.reserve(edge.local_columns.size());
	for (idx_t i = 0; i < edge.local_columns.size(); i++) {
		equalities.push_back("child." + QuoteIdentifier(edge.local_columns[i]) + " = parent." +
		                     QuoteIdentifier(edge.referenced_columns[i]));
	}
	if (ExecuteExists(connection,
	                  "SELECT 1 FROM " + child_table + " AS child WHERE NOT EXISTS (SELECT 1 FROM " + parent_table +
	                      " AS parent WHERE " + StringUtil::Join(equalities, " AND ") + ") LIMIT 1",
	                  "orphan-link check")) {
		throw InvalidInputException(FUNCTION_NAME + ": PRIVACY_LINK from '" + child.table_name + "' to '" +
		                            parent.table_name + "' contains an orphaned reference");
	}

	string grouped_columns = JoinQualifiedColumns("child", edge.local_columns, ", ");
	return ExecuteCount(connection,
	                    "SELECT COALESCE(MAX(frequency), 0) FROM (SELECT COUNT(*) AS frequency FROM " + child_table +
	                        " AS child GROUP BY " + grouped_columns + ") AS frequencies",
	                    "maximum frequency for " + child.table_name + " -> " + parent.table_name);
}

static OwnershipBounds ComputeNodeBounds(idx_t node_index, const OwnershipGraph &graph, vector<OwnershipBounds> &bounds,
                                         vector<bool> &computed) {
	if (computed[node_index]) {
		return bounds[node_index];
	}
	const auto &node = graph.nodes[node_index];
	if (node.is_pu) {
		bounds[node_index] = {1, 1};
		computed[node_index] = true;
		return bounds[node_index];
	}

	OwnershipBounds result {0, 0};
	for (auto edge_index : node.outgoing_edges) {
		const auto &edge = graph.edges[edge_index];
		auto parent_bounds = ComputeNodeBounds(edge.parent, graph, bounds, computed);
		// Sum every ownership path, including overlapping paths. This mirrors DP4SQL's
		// deliberately conservative maximum-ownership recurrence.
		auto owned_through_edge =
		    CheckedMultiply(edge.observed_max_frequency, parent_bounds.max_owned_rows, "maximum owned rows");
		result.max_owned_rows = CheckedAdd(result.max_owned_rows, owned_through_edge, "maximum owned rows");
		result.max_owners_per_row =
		    CheckedAdd(result.max_owners_per_row, parent_bounds.max_owners_per_row, "maximum owners per row");
	}
	bounds[node_index] = result;
	computed[node_index] = true;
	return result;
}

static void EnumeratePaths(idx_t node_index, const OwnershipGraph &graph, vector<idx_t> &path,
                           vector<vector<idx_t>> &paths) {
	if (node_index == graph.pu_node) {
		paths.push_back(path);
		return;
	}
	for (auto edge_index : graph.nodes[node_index].outgoing_edges) {
		path.push_back(edge_index);
		EnumeratePaths(graph.edges[edge_index].parent, graph, path, paths);
		path.pop_back();
	}
}

static string BuildPathOwnerQuery(const OwnershipGraph &graph, const vector<idx_t> &path) {
	const auto &fact = graph.nodes[graph.fact_node];
	const auto &pu = graph.nodes[graph.pu_node];
	string query = "SELECT " + JoinAliasedColumns("t0", fact.key_columns, "fact_key_") + ", " +
	               JoinAliasedColumns("t" + std::to_string(path.size()), pu.key_columns, "pu_key_") + " FROM " +
	               QuoteIdentifier(fact.table_name) + " AS t0";
	for (idx_t depth = 0; depth < path.size(); depth++) {
		const auto &edge = graph.edges[path[depth]];
		const auto &parent = graph.nodes[edge.parent];
		string child_alias = "t" + std::to_string(depth);
		string parent_alias = "t" + std::to_string(depth + 1);
		vector<string> conditions;
		conditions.reserve(edge.local_columns.size());
		for (idx_t i = 0; i < edge.local_columns.size(); i++) {
			conditions.push_back(child_alias + "." + QuoteIdentifier(edge.local_columns[i]) + " = " + parent_alias +
			                     "." + QuoteIdentifier(edge.referenced_columns[i]));
		}
		query += " INNER JOIN " + QuoteIdentifier(parent.table_name) + " AS " + parent_alias + " ON " +
		         StringUtil::Join(conditions, " AND ");
	}
	return query;
}

static uint64_t ComputeObservedMaxAffectedPus(Connection &connection, const OwnershipGraph &graph) {
	vector<vector<idx_t>> paths;
	vector<idx_t> path;
	EnumeratePaths(graph.fact_node, graph, path, paths);
	if (paths.empty()) {
		throw InternalException(FUNCTION_NAME + ": ownership graph contains no path to its privacy unit");
	}

	vector<string> path_queries;
	path_queries.reserve(paths.size());
	for (const auto &ownership_path : paths) {
		path_queries.push_back(BuildPathOwnerQuery(graph, ownership_path));
	}
	const auto &fact = graph.nodes[graph.fact_node];
	const auto &pu = graph.nodes[graph.pu_node];
	vector<string> fact_equalities;
	fact_equalities.reserve(fact.key_columns.size());
	for (idx_t i = 0; i < fact.key_columns.size(); i++) {
		string column = QuoteIdentifier("fact_key_" + std::to_string(i));
		fact_equalities.push_back("left_owner." + column + " = right_owner." + column);
	}
	vector<string> different_owner;
	different_owner.reserve(pu.key_columns.size());
	vector<string> left_owner_columns;
	vector<string> right_owner_columns;
	left_owner_columns.reserve(pu.key_columns.size());
	right_owner_columns.reserve(pu.key_columns.size());
	for (idx_t i = 0; i < pu.key_columns.size(); i++) {
		string column = QuoteIdentifier("pu_key_" + std::to_string(i));
		different_owner.push_back("left_owner." + column + " <> right_owner." + column);
		left_owner_columns.push_back("left_owner." + column + " AS " + QuoteIdentifier("left_pu_" + std::to_string(i)));
		right_owner_columns.push_back("right_owner." + column + " AS " +
		                              QuoteIdentifier("right_pu_" + std::to_string(i)));
	}
	vector<string> degree_columns;
	degree_columns.reserve(pu.key_columns.size());
	for (idx_t i = 0; i < pu.key_columns.size(); i++) {
		degree_columns.push_back(QuoteIdentifier("left_pu_" + std::to_string(i)));
	}

	string query = "WITH owners_raw AS (" + StringUtil::Join(path_queries, " UNION ALL ") +
	               "), owners AS (SELECT DISTINCT * FROM owners_raw), pairs AS (SELECT DISTINCT " +
	               StringUtil::Join(left_owner_columns, ", ") + ", " + StringUtil::Join(right_owner_columns, ", ") +
	               " FROM owners AS left_owner INNER JOIN owners AS right_owner ON " +
	               StringUtil::Join(fact_equalities, " AND ") + " WHERE " + StringUtil::Join(different_owner, " OR ") +
	               "), degrees AS (SELECT " + StringUtil::Join(degree_columns, ", ") +
	               ", COUNT(*) AS degree FROM pairs GROUP BY " + StringUtil::Join(degree_columns, ", ") +
	               ") SELECT COALESCE(MAX(degree), 0) FROM degrees";
	// Directed pairs avoid ordering or hashing composite PU keys. Each left key's
	// pair count is exactly its number of distinct co-owners.
	return ExecuteCount(connection, query, "observed maximum affected privacy units");
}

} // namespace

DPOwnershipAnalysis AnalyzeObservedDPOwnershipGraph(ClientContext &context, const string &fact_table_input) {
	string fact_table = NormalizeTableName(fact_table_input);
	if (fact_table.empty()) {
		throw InvalidInputException(FUNCTION_NAME + ": fact table name must not be empty");
	}
	PRIVACY_DEBUG_PRINT("[dp_ownership_graph] building graph for fact table '" + fact_table + "'");
	auto graph = OwnershipGraphBuilder(fact_table).Build();

	auto &database = DatabaseInstance::GetDatabase(context);
	Connection connection(database);
	RunSetting(connection, "SET priv_rewrite = false");

	ValidateKey(connection, graph.nodes[graph.fact_node].table_name, graph.nodes[graph.fact_node].key_columns,
	            "fact PRIVACY_KEY");
	ValidateKey(connection, graph.nodes[graph.pu_node].table_name, graph.nodes[graph.pu_node].key_columns,
	            "privacy-unit PRIVACY_KEY");
	std::unordered_set<string> validated_parent_keys;
	for (auto &edge : graph.edges) {
		const auto &parent = graph.nodes[edge.parent];
		string key_signature = parent.table_name + "\x1f" + StringUtil::Join(edge.referenced_columns, "\x1f");
		if (validated_parent_keys.insert(key_signature).second) {
			ValidateKey(connection, parent.table_name, edge.referenced_columns, "referenced PRIVACY_LINK key");
		}
		edge.observed_max_frequency = ValidateLinkAndGetMaxFrequency(connection, graph, edge);
	}

	vector<OwnershipBounds> node_bounds(graph.nodes.size());
	vector<bool> computed(graph.nodes.size(), false);
	auto root_bounds = ComputeNodeBounds(graph.fact_node, graph, node_bounds, computed);
	uint64_t graph_max_affected = CheckedMultiply(root_bounds.max_owned_rows, root_bounds.max_owners_per_row - 1,
	                                              "maximum affected privacy units");
	uint64_t observed_max_affected = ComputeObservedMaxAffectedPus(connection, graph);
	if (observed_max_affected > graph_max_affected) {
		throw InternalException(FUNCTION_NAME + ": observed PU degree exceeds the graph-derived upper bound");
	}

	DPOwnershipAnalysis result;
	result.fact_table = graph.nodes[graph.fact_node].table_name;
	result.pu_table = graph.nodes[graph.pu_node].table_name;
	result.graph_max_affected_pus = graph_max_affected;
	result.observed_max_affected_pus = observed_max_affected;
	result.edges.reserve(graph.edges.size());
	for (const auto &edge : graph.edges) {
		const auto &child_bounds = node_bounds[edge.child];
		result.edges.push_back({graph.nodes[edge.child].table_name, edge.local_columns,
		                        graph.nodes[edge.parent].table_name, edge.referenced_columns,
		                        edge.observed_max_frequency, child_bounds.max_owned_rows,
		                        child_bounds.max_owners_per_row});
	}
	PRIVACY_DEBUG_PRINT("[dp_ownership_graph] graph bound=" + std::to_string(graph_max_affected) +
	                    " observed degree=" + std::to_string(observed_max_affected));
	return result;
}

} // namespace duckdb
