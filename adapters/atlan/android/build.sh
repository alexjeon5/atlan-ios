#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# adapter: builds the Atlan arm64-v8a APK (platform/android/build_apk.sh; options pass through, e.g. --asset FILE).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$here/../../../platform/android/build_apk.sh" --app "$here" "$@"
