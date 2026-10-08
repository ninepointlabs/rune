// Formatting commands: .uno: pass-through, color, paragraph style and
// spacing, cached state and the format painter.

#include "engine_internal.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rune {

namespace {

// `style` names -> Writer programmatic paragraph style names for
// .uno:StyleApply (UI names like "Body Text" are not found). STATE_CHANGED
// reports StyleApply with UI names: "Default Paragraph Style", "Body Text".
const std::map<std::string, const char *> kParaStyles = {
    {"Normal", "Standard"}, {"Text Body", "Text body"},
    {"Heading 1", "Heading 1"}, {"Heading 2", "Heading 2"}, {"Heading 3", "Heading 3"},
};

// `format` args -> the LOK typed-argument JSON postUnoCommand expects. A
// JSON object passes through; a plain value is wrapped for the commands
// that take one (font name, size in points).
bool unoArgs(const std::string &command, const std::string &value, std::string &out, std::string &error)
{
    if (!value.empty() && value[0] == '{') {
        Json j;
        if (!JsonParser(value).parse(j) || j.type != Json::Object) {
            error = "\"args\" is not a JSON object";
            return false;
        }
        out = value;
        return true;
    }
    if (command == ".uno:CharFontName") {
        if (value.empty()) {
            error = "empty font name";
            return false;
        }
        out = "{\"CharFontName.FamilyName\":{\"type\":\"string\",\"value\":" + jsonQuote(value) + "}}";
        return true;
    }
    if (command == ".uno:FontHeight") {
        char *end = nullptr;
        const double pt = std::strtod(value.c_str(), &end);
        if (value.empty() || *end != '\0' || !(pt >= 1 && pt <= 999)) {
            error = "invalid font size: " + value;
            return false;
        }
        out = "{\"FontHeight.Height\":{\"type\":\"float\",\"value\":" + jsonQuote(value) + "}}";
        return true;
    }
    error = "plain \"args\" not supported for " + command + "; pass a JSON object";
    return false;
}

// "#RRGGBB" -> R*65536 + G*256 + B; "auto" -> -1 (COL_AUTO as a UNO long).
bool parseColor(const std::string &hex, long long &value)
{
    if (hex == "auto") {
        value = -1;
        return true;
    }
    if (hex.size() != 7 || hex[0] != '#'
        || hex.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
        return false;
    value = std::stoll(hex.substr(1), nullptr, 16);
    return true;
}

} // namespace

// Pass-through to LOK; it runs asynchronously, so an unknown or
// inapplicable command still replies ok. Only .uno: commands, so a
// client can't dispatch macro: or script URLs.
std::string Engine::format(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    const Json *command = req.get("command");
    if (!command || command->type != Json::String || command->s.rfind(".uno:", 0) != 0
        || command->s.size() == 5)
        return errorReply(id, "\"command\" must be a .uno: command");
    const Json *argsField = req.get("args");
    if (!argsField) {
        st->doc->postUnoCommand(command->s.c_str());
    } else {
        if (argsField->type != Json::String)
            return errorReply(id, "\"args\" must be a string");
        std::string args, error;
        if (!unoArgs(command->s, argsField->s, args, error))
            return errorReply(id, error);
        st->doc->postUnoCommand(command->s.c_str(), args.c_str());
    }
    st->dirty = true;
    return Reply(id).ok(true).line();
}

// Sets the character color: "#RRGGBB", or "auto" for the automatic color.
std::string Engine::color(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    const Json *hex = req.get("hex");
    if (!hex || hex->type != Json::String)
        return errorReply(id, "missing \"hex\"");
    long long value;
    if (!parseColor(hex->s, value))
        return errorReply(id, "invalid \"hex\": " + hex->s + " (#RRGGBB or auto)");
    setColor(*st, value);
    st->dirty = true;
    return Reply(id).ok(true).line();
}

// value: R*65536 + G*256 + B, or -1 for the automatic color.
void Engine::setColor(DocState &st, long long value)
{
    const std::string args = "{\"Color\":{\"type\":\"long\",\"value\":" + std::to_string(value) + "}}";
    st.doc->postUnoCommand(".uno:Color", args.c_str());
}

// Applies a paragraph style to the paragraph(s) at the cursor.
std::string Engine::style(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    const Json *name = req.get("name");
    if (!name || name->type != Json::String)
        return errorReply(id, "missing \"name\"");
    auto it = kParaStyles.find(name->s);
    if (it == kParaStyles.end())
        return errorReply(id, "unknown style: " + name->s
                                  + " (Normal, Heading 1, Heading 2, Heading 3, Text Body)");
    const std::string args = std::string("{\"Style\":{\"type\":\"string\",\"value\":")
        + jsonQuote(it->second)
        + "},\"FamilyName\":{\"type\":\"string\",\"value\":\"ParagraphStyles\"}}";
    st->doc->postUnoCommand(".uno:StyleApply", args.c_str());
    st->dirty = true;
    return Reply(id).ok(true).line();
}

// Paragraph spacing, directional only (space_before/space_after exact
// point values and keep-together/keep-with-next/widow-orphan are NOT
// implemented: LibreOfficeKit's postUnoCommand(".uno:ParagraphDialog",
// args) does not apply ParaTopMargin/ParaBottomMargin/ParaSplit in
// headless mode -- verified empirically by diffing rendered tiles
// before/after the call, which showed no change for any of those
// properties. ParagraphDialog is a dialog-driving command; without a
// GUI event loop behind it, LOK accepts the call (postUnoCommand never
// reports failure) but the properties are never actually applied. Only
// the simple toggle dispatch commands ParaspaceIncrease/ParaspaceDecrease
// were confirmed to work (visually, via tile diff). Exact paragraph
// dialog settings would need the full UNO API (XPropertySet on a text
// cursor), which is a different, heavier integration than LOK's
// simple command dispatch.
std::string Engine::para(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    const Json *dir = req.get("direction");
    if (!dir || dir->type != Json::String)
        return errorReply(id, "missing \"direction\" (\"increase\" or \"decrease\")");
    if (dir->s == "increase")
        st->doc->postUnoCommand(".uno:ParaspaceIncrease");
    else if (dir->s == "decrease")
        st->doc->postUnoCommand(".uno:ParaspaceDecrease");
    else
        return errorReply(id, "\"direction\" must be \"increase\" or \"decrease\"");
    st->dirty = true;
    return Reply(id).ok(true).line();
}

// Formatting state at the cursor, cached from STATE_CHANGED callbacks.
std::string Engine::getState(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    std::string state = "{";
    for (const auto &[key, value] : st->state) {
        if (state.size() > 1)
            state += ',';
        state += jsonQuote(key) + ':' + value;
    }
    return Reply(id).ok(true).raw("state", state + "}").line();
}

// Character + paragraph formatting snapshot at the cursor, from the same
// STATE_CHANGED cache as get_state. Color is a number (-1 = automatic).
std::string Engine::getCharStyle(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    std::vector<const char *> keys(std::begin(kToggleKeys), std::end(kToggleKeys));
    keys.insert(keys.end(), {"CharFontName", "FontHeight", "Color"});
    keys.insert(keys.end(), std::begin(kAlignKeys), std::end(kAlignKeys));
    std::string style = "{";
    for (const char *key : keys) {
        auto it = st->state.find(key);
        if (it == st->state.end())
            continue;
        std::string value = it->second;
        long long color;
        if (std::strcmp(key, "Color") == 0) {
            if (!parseStateColor(value, color))
                continue; // e.g. "disabled"
            value = std::to_string(color);
        }
        if (style.size() > 1)
            style += ',';
        style += jsonQuote(key) + ':' + value;
    }
    return Reply(id).ok(true).raw("style", style + "}").line();
}

// A cached Color value ("\"16711680\"") -> number; false if not one.
bool Engine::parseStateColor(const std::string &json, long long &out)
{
    if (json.size() < 3 || json.front() != '"' || json.back() != '"')
        return false;
    const std::string s = json.substr(1, json.size() - 2);
    auto [next, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && next == s.data() + s.size();
}

// Applies a get_char_style snapshot to the selection. Unknown keys are
// ignored and empty values (mixed selection) skipped; everything is
// validated before any command is sent.
std::string Engine::applyCharStyle(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    const Json *style = req.get("style");
    if (!style || style->type != Json::Object)
        return errorReply(id, "\"style\" must be an object");

    std::vector<std::pair<std::string, std::string>> commands; // (command, args or "")
    for (const char *key : kToggleKeys) {
        const Json *v = style->get(key);
        if (!v)
            continue;
        if (v->type != Json::Bool)
            return errorReply(id, std::string("\"") + key + "\" must be true or false");
        auto it = st->state.find(key);
        const bool current = it != st->state.end() && it->second == "true";
        if (current != v->b)
            commands.emplace_back(std::string(".uno:") + key, "");
    }
    if (const Json *v = style->get("CharFontName")) {
        if (v->type != Json::String)
            return errorReply(id, "\"CharFontName\" must be a string");
        std::string args, error;
        if (!v->s.empty()) {
            if (!unoArgs(".uno:CharFontName", v->s, args, error))
                return errorReply(id, error);
            commands.emplace_back(".uno:CharFontName", args);
        }
    }
    if (const Json *v = style->get("FontHeight")) {
        std::string size;
        if (v->type == Json::String) {
            size = v->s;
        } else if (v->type == Json::Number) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", v->n);
            size = buf;
        } else {
            return errorReply(id, "\"FontHeight\" must be a string or number");
        }
        std::string args, error;
        if (!size.empty()) {
            if (!unoArgs(".uno:FontHeight", size, args, error))
                return errorReply(id, error);
            commands.emplace_back(".uno:FontHeight", args);
        }
    }
    long long color = 0;
    bool hasColor = false;
    if (const Json *v = style->get("Color")) {
        if (v->type != Json::Number || std::trunc(v->n) != v->n || v->n < -1 || v->n > 0xFFFFFF)
            return errorReply(id, "\"Color\" must be -1 (automatic) or 0..16777215");
        color = static_cast<long long>(v->n);
        hasColor = true;
    }
    for (const char *key : kAlignKeys) {
        const Json *v = style->get(key);
        if (v && v->type != Json::Bool)
            return errorReply(id, std::string("\"") + key + "\" must be true or false");
        if (v && v->b) {
            commands.emplace_back(std::string(".uno:") + key, "");
            break;
        }
    }

    for (const auto &[command, args] : commands) {
        if (args.empty())
            st->doc->postUnoCommand(command.c_str());
        else
            st->doc->postUnoCommand(command.c_str(), args.c_str());
    }
    if (hasColor)
        setColor(*st, color);
    // Toggles are async: record the target now so a second apply before
    // STATE_CHANGED arrives does not flip them back.
    for (const char *key : kToggleKeys)
        if (const Json *v = style->get(key))
            st->state[key] = v->b ? "true" : "false";
    if (!commands.empty() || hasColor)
        st->dirty = true;
    return Reply(id).ok(true).line();
}

} // namespace rune
