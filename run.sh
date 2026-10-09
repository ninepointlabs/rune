#!/bin/bash
# Launch Rune (the native, single-process app).
#
#   ./run.sh                    # blank document
#   ./run.sh path/to/doc.docx   # open a specific file

cd "$(dirname "${BASH_SOURCE[0]}")"
exec ./build/native/rune "$@"