#pragma once
// Record tables the registry keeps for the palette: quicklinks, snippets,
// custom commands, window sizes, and each palette entry's own preferences
// (alias, favorite, hidden). One typed column per field, described here once;
// the registry creates the tables and adds a column a newer atrium describes,
// and the IPC lists, adds, changes and removes records by these descriptions.

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atrium {

using json = nlohmann::json;

enum class ColumnType { Text, Integer, Real, Bool };

struct Column {
    const char* name;
    ColumnType type;
    json fallback;          // what a new record has when not told
    bool nullable = false;  // null is a value of its own ("not a favorite")
};

struct RecordTable {
    const char* name;      // "quicklinks": the SQL table, "<name>.list"
    const char* singular;  // "quicklink": what a request names a record by
    // Records keyed by a text column (the entry preferences, by entry key),
    // or "" for numbered ones (INTEGER PRIMARY KEY "id", kept in order).
    const char* key;
    std::vector<Column> columns;
};

const std::vector<RecordTable>& record_tables();
const RecordTable* record_table(std::string_view name);
// A request's fields checked against the table: the first wrong one, or nothing.
std::optional<std::string> check_record_fields(const RecordTable& table, const json& fields);

} // namespace atrium
