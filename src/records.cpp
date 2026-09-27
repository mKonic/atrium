#include "records.hpp"

namespace atrium {

const std::vector<RecordTable>& record_tables() {
    using enum ColumnType;
    static const std::vector<RecordTable> tables = {
        // A palette entry's own preferences, by its key ("app:firefox.desktop",
        // "quicklink:3", "system:sleep").
        {"launcher_entries", "launcher_entry", "key",
         {{"alias", Text, ""}, {"favorite", Integer, nullptr, true}, {"hidden", Bool, false}}},
        // A web address, search, file or folder with {placeholders}, opened
        // with the default app or the one named (a desktop entry id).
        {"quicklinks", "quicklink", "",
         {{"name", Text, ""}, {"url", Text, ""}, {"app", Text, ""}, {"icon", Text, ""}, {"root", Bool, true}}},
        // Text with {placeholders}, typed where the cursor is; the keyword
        // types it too when snippet expansion is on.
        {"snippets", "snippet", "", {{"name", Text, ""}, {"text", Text, ""}, {"keyword", Text, ""}}},
        // A named shell command. $1 $2 $3 are asked for; output shows what it
        // printed; confirm asks first; terminal runs it in the terminal.
        {"commands", "command", "",
         {{"name", Text, ""}, {"command", Text, ""}, {"directory", Text, ""}, {"icon", Text, ""},
          {"output", Bool, false}, {"confirm", Bool, false}, {"terminal", Bool, false}}},
        // A window size of the user's own, as a share of the screen, centered.
        {"window_sizes", "window_size", "", {{"name", Text, ""}, {"width", Real, 0.6}, {"height", Real, 0.6}}},
    };
    return tables;
}

const RecordTable* record_table(std::string_view name) {
    for (const RecordTable& t : record_tables())
        if (name == t.name || name == t.singular)
            return &t;
    return nullptr;
}

std::optional<std::string> check_record_fields(const RecordTable& table, const json& fields) {
    if (!fields.is_object())
        return std::string("fields are an object");
    for (const Column& c : table.columns) {
        if (!fields.contains(c.name))
            continue;
        const json& v = fields[c.name];
        if (v.is_null() && c.nullable)
            continue;
        bool fits = false;
        switch (c.type) {
        case ColumnType::Text: fits = v.is_string(); break;
        case ColumnType::Integer: fits = v.is_number_integer(); break;
        case ColumnType::Real: fits = v.is_number(); break;
        case ColumnType::Bool: fits = v.is_boolean(); break;
        }
        if (!fits) {
            static const char* kinds[] = {"text", "a whole number", "a number", "true or false"};
            return std::string(c.name) + " is " + kinds[int(c.type)] + (c.nullable ? " or null" : "");
        }
    }
    return std::nullopt;
}

} // namespace atrium
