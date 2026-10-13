#pragma once

// DocxBridge: .docx (and anything else LibreOffice reads) in and out of Rune
// by one-shot batch conversion with a headless `soffice`. LibreOffice is
// never in the editing path: opening converts to a temporary .odt that
// OdfReader loads; saving writes a temporary .odt that is converted to the
// target format.
//
// soffice runs with a private user profile (~/.cache/rune/lo-profile, or
// the platform's equivalent) so it doesn't attach to, or wait on, a
// LibreOffice the user has open. Conversions are synchronous and block the
// caller for up to kTimeoutMs; a timed-out soffice is killed.
//
// Conversions are not serialised: two running at once share the profile,
// and the second soffice may hand its job to the first instance (it still
// completes, but the timeouts no longer apply independently).

#include <QString>

namespace DocxBridge {

// Upper bound on one soffice run. The first run with a fresh profile builds
// it (several seconds); later runs take ~1-3 s.
constexpr int kTimeoutMs = 60000;

// Converts any format soffice understands (.docx, .doc, .rtf, ...) at
// `sourcePath` to a fresh .odt in a new private temporary directory.
// Returns the .odt's path on success; on failure returns "" with `*error`
// (if non-null) set, and nothing is left behind.
//
// Ownership: the directory holding the returned .odt is the caller's; pass
// the path to removeConvertedOdt() when done with it. (A bare QString can't
// carry a QTemporaryDir's lifetime, and handing the caller the directory
// object would leak QTemporaryDir into every call site for one use.)
QString convertToOdt(const QString &sourcePath, QString *error = nullptr);

// Deletes a convertToOdt() result and its temporary directory. Does nothing
// for a path that isn't one (wrong directory name), so a stray call can't
// remove anything else.
void removeConvertedOdt(const QString &odtPath);

// Converts the .odt at `odtPath` to Word 2007+ .docx at `targetPath`.
// Returns true on success; on failure returns false with `*error` set and
// `targetPath` untouched (it is replaced atomically, only once soffice has
// produced the output).
bool convertOdtToDocx(const QString &odtPath, const QString &targetPath, QString *error = nullptr);

} // namespace DocxBridge
