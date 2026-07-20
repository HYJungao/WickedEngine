#!/bin/sh
set -eu

OIDN_ROOT="$1"
DESTINATION="$2"
HEADER="$OIDN_ROOT/include/OpenImageDenoise/oidn.hpp"
LINK_LIBRARY="$OIDN_ROOT/lib/libOpenImageDenoise.dylib"

if [ ! -f "$HEADER" ] || [ ! -f "$LINK_LIBRARY" ]; then
    echo "error: incomplete OpenImageDenoise SDK at $OIDN_ROOT" >&2
    echo "error: extract the official macOS package contents directly into that directory" >&2
    exit 1
fi

mkdir -p "$DESTINATION"
copied=0
signing_allowed="${CODE_SIGNING_ALLOWED:-NO}"
signing_identity="${EXPANDED_CODE_SIGN_IDENTITY:--}"
for directory in "$OIDN_ROOT/lib" "$OIDN_ROOT/bin"; do
    for source in "$directory"/*.dylib; do
        if [ ! -f "$source" ]; then
            continue
        fi
        # Resolve package symlinks while retaining every ABI filename. This
        # keeps @rpath references valid inside a relocatable application bundle.
        destination="$DESTINATION/$(basename "$source")"
        cp -fL "$source" "$destination"
        if [ "$signing_allowed" = "YES" ]; then
            codesign --force --sign "$signing_identity" --timestamp=none "$destination"
        fi
        copied=1
    done
done

if [ "$copied" -ne 1 ]; then
    echo "error: OpenImageDenoise SDK contains no macOS runtime dylibs" >&2
    exit 1
fi
