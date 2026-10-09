// Table commands: insert, AutoFit, row insert/delete.
//
// Repeat-header-rows is NOT implemented: .uno:SetRowRepeatHeading,
// .uno:TableRepeatHeading and .uno:SetRepeatHeadline were each tried via
// postUnoCommand and none changed the cached state or the rendered tile.
// The table's RepeatHeadline/HeaderRowCount properties are only reachable
// through the full UNO API (XPropertySet on the text table), the same
// class of limitation as exact paragraph spacing (see Engine::para).

#include "engine_internal.h"

namespace rune {

namespace {

constexpr long long kMaxTableDim = 50;

// The cursor is in a table cell when STATE_CHANGED has reported a
// non-empty StateTableCell ("\"Table1:A1\""); it is "\"\"" or absent
// outside one.
bool inTable(const std::map<std::string, std::string> &state)
{
    auto it = state.find("StateTableCell");
    return it != state.end() && !it->second.empty() && it->second != "\"\"" && it->second != "null";
}

} // namespace

std::string Engine::table(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    const Json *action = req.get("action");
    if (!action || action->type != Json::String)
        return errorReply(id, "missing \"action\" (insert, autofit, insert_row, delete_row, insert_column, delete_column)");

    if (action->s == "insert") {
        long long rows, columns;
        if (!getInt(req, "rows", rows) || rows < 1 || rows > kMaxTableDim)
            return errorReply(id, "\"rows\" must be an integer 1..50");
        if (!getInt(req, "columns", columns) || columns < 1 || columns > kMaxTableDim)
            return errorReply(id, "\"columns\" must be an integer 1..50");
        const std::string args = "{\"Columns\":{\"type\":\"short\",\"value\":" + std::to_string(columns)
            + "},\"Rows\":{\"type\":\"short\",\"value\":" + std::to_string(rows) + "}}";
        st->doc->postUnoCommand(".uno:InsertTable", args.c_str());
        st->dirty = true;
        return Reply(id).ok(true).line();
    }

    const char *command;
    if (action->s == "autofit")
        command = nullptr; // two commands, below
    else if (action->s == "insert_row")
        command = ".uno:InsertRowsAfter";
    else if (action->s == "delete_row")
        command = ".uno:DeleteRows";
    else if (action->s == "insert_column")
        command = ".uno:InsertColumnsAfter";
    else if (action->s == "delete_column")
        command = ".uno:DeleteColumns";
    else
        return errorReply(id, "unknown \"action\": " + action->s
            + " (insert, autofit, insert_row, delete_row, insert_column, delete_column)");

    if (!inTable(st->state))
        return errorReply(id, "cursor is not inside a table");
    if (command) {
        st->doc->postUnoCommand(command);
    } else {
        // SetOptimalColumnWidth only acts on selected cells; alone it is a no-op.
        st->doc->postUnoCommand(".uno:SelectTable");
        st->doc->postUnoCommand(".uno:SetOptimalColumnWidth");
    }
    st->dirty = true;
    return Reply(id).ok(true).line();
}

} // namespace rune
