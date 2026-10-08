// Live text editing: keyboard, mouse, paste and copy/cut.

#include "engine_internal.h"

#include <climits>
#include <cstdlib>

namespace rune {

namespace {

// VCL key codes (vcl/keycodes.hxx, == css::awt::Key), which is not installed
// with the SDK. Values checked against offapi.rdb for LO 26.8.
const std::map<std::string, int> kKeyNames = {
    {"Down", 1024},     {"Up", 1025},     {"Left", 1026},   {"Right", 1027},
    {"Home", 1028},     {"End", 1029},    {"PageUp", 1030}, {"PageDown", 1031},
    {"Return", 1280},   {"Enter", 1280},  {"Escape", 1281}, {"Tab", 1282},
    {"Backspace", 1283}, {"Space", 1284}, {"Insert", 1285}, {"Delete", 1286},
};

} // namespace

std::string Engine::key(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    lok::Document *doc = st->doc.get();

    const Json *type = req.get("type");
    int lokType;
    if (type && type->type == Json::String && type->s == "input")
        lokType = LOK_KEYEVENT_KEYINPUT;
    else if (type && type->type == Json::String && type->s == "up")
        lokType = LOK_KEYEVENT_KEYUP;
    else
        return errorReply(id, "\"type\" must be \"input\" or \"up\"");

    long long charCode = 0, keyCode = 0;
    if ((req.get("char_code") && !getInt(req, "char_code", charCode))
        || (req.get("key_code") && !getInt(req, "key_code", keyCode)) || charCode < 0
        || charCode > 0x10FFFF || keyCode < 0 || keyCode > 0xFFFF)
        return errorReply(id, "invalid char_code/key_code");
    // Convenience for hand testing: a key name instead of a VCL code.
    if (const Json *name = req.get("key")) {
        auto it = name->type == Json::String ? kKeyNames.find(name->s) : kKeyNames.end();
        if (it == kKeyNames.end())
            return errorReply(id, "unknown \"key\" name");
        keyCode |= it->second;
    }
    if (charCode == 0 && keyCode == 0)
        return errorReply(id, "char_code or key_code is required");

    doc->postKeyEvent(lokType, int(charCode), int(keyCode));
    // Conservative: navigation keys count too; at worst an extra autosave.
    if (lokType == LOK_KEYEVENT_KEYINPUT)
        st->dirty = true;
    return Reply(id).ok(true).line();
}

std::string Engine::paste(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    lok::Document *doc = st->doc.get();
    const Json *mime = req.get("mime_type");
    const Json *data = req.get("data");
    if (!data || data->type != Json::String)
        return errorReply(id, "missing \"data\"");
    std::string mimeType = mime && mime->type == Json::String ? mime->s : "";
    // LO only recognises plain text with an explicit charset.
    if (mimeType.empty() || mimeType == "text/plain")
        mimeType = "text/plain;charset=utf-8";
    if (!doc->paste(mimeType.c_str(), data->s.data(), data->s.size()))
        return errorReply(id, "paste failed for " + mimeType);
    st->dirty = true;
    return Reply(id).ok(true).line();
}

// x/y in document twips, like tile and cursor coordinates.
std::string Engine::mouse(const Json &req, const Json *id)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    lok::Document *doc = st->doc.get();

    const Json *type = req.get("type");
    int lokType;
    if (type && type->type == Json::String && type->s == "down")
        lokType = LOK_MOUSEEVENT_MOUSEBUTTONDOWN;
    else if (type && type->type == Json::String && type->s == "move")
        lokType = LOK_MOUSEEVENT_MOUSEMOVE;
    else if (type && type->type == Json::String && type->s == "up")
        lokType = LOK_MOUSEEVENT_MOUSEBUTTONUP;
    else
        return errorReply(id, "\"type\" must be \"down\", \"move\" or \"up\"");

    long long x, y, count = 1, buttons = 1, modifiers = 0;
    if (!getInt(req, "x", x) || !getInt(req, "y", y) || x < INT_MIN || x > INT_MAX
        || y < INT_MIN || y > INT_MAX)
        return errorReply(id, "\"x\" and \"y\" must be integers");
    if ((req.get("count") && !getInt(req, "count", count))
        || (req.get("buttons") && !getInt(req, "buttons", buttons))
        || (req.get("modifiers") && !getInt(req, "modifiers", modifiers)) || count < 1
        || count > 3 || buttons < 0 || buttons > 0xFFFF || modifiers < 0 || modifiers > 0xFFFF)
        return errorReply(id, "invalid count/buttons/modifiers");

    doc->postMouseEvent(lokType, int(x), int(y), int(count), int(buttons), int(modifiers));
    return Reply(id).ok(true).line();
}

// Selected text as UTF-8; `cut` then deletes the selection.
std::string Engine::copy(const Json &req, const Json *id, bool cut)
{
    long long docId;
    DocState *st = findState(req, docId);
    if (!st)
        return errorReply(id, "unknown doc_id");
    lok::Document *doc = st->doc.get();

    std::string text;
    if (char *sel = doc->getTextSelection("text/plain;charset=utf-8", nullptr)) {
        text = sel;
        std::free(sel);
    }
    if (cut && !text.empty()) {
        const int del = kKeyNames.at("Delete");
        doc->postKeyEvent(LOK_KEYEVENT_KEYINPUT, 0, del);
        doc->postKeyEvent(LOK_KEYEVENT_KEYUP, 0, del);
        st->dirty = true;
    }
    return Reply(id).ok(true).str("text", text).line();
}

} // namespace rune
