#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
repo_root="$(cd "$script_dir/../../.." && pwd -P)"
project_cmake="$repo_root/demo/project.cmake"
profiles_dir="$HOME/Library/MobileDevice/Provisioning Profiles"
tmp_dir="$(mktemp -d)"
found_ios_profile=0
found_matching_ios_profile=0
issues=()
identity_output=""
identity_list=""
profile_summaries=()
expired_matches=()
expected_team=""
expected_bundle=""
# Plist dates are ISO-8601 UTC, which compare correctly as plain strings.
now_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cleanup() {
  rm -rf "$tmp_dir"
}

trap cleanup EXIT

if [[ -f "$project_cmake" ]]; then
  expected_team="$(sed -n 's/.*GLINT_IOS_DEVELOPMENT_TEAM "\([^"]*\)".*/\1/p' "$project_cmake" | head -n 1)"
  expected_bundle="$(sed -n 's/.*GLINT_IOS_BUNDLE_IDENTIFIER "\([^"]*\)".*/\1/p' "$project_cmake" | head -n 1)"
fi

identity_output="$(security find-identity -v -p codesigning 2>/dev/null || true)"
identity_list="$(printf '%s\n' "$identity_output" | sed -n 's/.*"\([^"]*\)"/  - \1/p')"

if ! printf '%s\n' "$identity_output" | grep -Eq 'Apple Development|iPhone Developer'; then
  issues+=("No iOS development signing identity is installed on this Mac.")
fi

if [[ ! -d "$profiles_dir" ]]; then
  issues+=("No provisioning profiles directory was found.")
else
  for profile in "$profiles_dir"/*.provisionprofile(.N) "$profiles_dir"/*.mobileprovision(.N); do
    decoded="$tmp_dir/${profile:t}.plist"
    if ! security cms -D -i "$profile" > "$decoded" 2>/dev/null; then
      continue
    fi

    name="$(/usr/libexec/PlistBuddy -c 'Print :Name' "$decoded" 2>/dev/null || echo "${profile:t}")"
    platform="$(/usr/libexec/PlistBuddy -c 'Print :Platform:0' "$decoded" 2>/dev/null || true)"
    platforms="$(/usr/libexec/PlistBuddy -c 'Print :Platform' "$decoded" 2>/dev/null || true)"
    team="$(/usr/libexec/PlistBuddy -c 'Print :TeamIdentifier:0' "$decoded" 2>/dev/null || true)"
    app_id="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:application-identifier' "$decoded" 2>/dev/null || true)"
    expires="$(plutil -extract ExpirationDate xml1 -o - "$decoded" 2>/dev/null | sed -n 's:.*<date>\(.*\)</date>.*:\1:p' || true)"
    expired=0
    expiry_note=""
    if [[ -n "$expires" && "$expires" < "$now_utc" ]]; then
      expired=1
      expiry_note=" (EXPIRED)"
    fi

    profile_summaries+=("  - ${name}: platform=${platform:-unknown}, team=${team:-unknown}, app-id=${app_id:-unknown}, expires=${expires:-unknown}${expiry_note}")

    if ! printf '%s\n' "$platforms" | grep -Eq 'iOS|iPhoneOS'; then
      continue
    fi

    found_ios_profile=1

    if [[ -n "$expected_team" && "$team" != "$expected_team" ]]; then
      continue
    fi

    # application-identifier is "<team>.<bundle id>", and the bundle part may
    # be a wildcard: "*" or e.g. "io.superkraft.*".
    if [[ -n "$expected_bundle" && -n "$app_id" ]]; then
      app_bundle="${app_id#*.}"
      if [[ "$expected_bundle" != ${~app_bundle} ]]; then
        continue
      fi
    fi

    # Signing with an expired profile fails at build time.
    if (( expired )); then
      expired_matches+=("${name} (expired ${expires})")
      continue
    fi

    found_matching_ios_profile=1
    break
  done
fi

if [[ "$found_ios_profile" -eq 0 ]]; then
  issues+=("No iOS provisioning profile is installed on this Mac.")
elif [[ "$found_matching_ios_profile" -eq 0 ]]; then
  if (( ${#expired_matches[@]} > 0 )); then
    issues+=("Every iOS provisioning profile for team ${expected_team:-<unset>} and bundle ${expected_bundle:-<unset>} has expired: ${(j:, :)expired_matches}.")
  else
    issues+=("No installed iOS provisioning profile matches team ${expected_team:-<unset>} and bundle ${expected_bundle:-<unset>}.")
  fi
fi

if (( ${#issues[@]} > 0 )); then
  printf '%s\n' "${issues[@]}"
  echo
  if [[ -n "$expected_team" || -n "$expected_bundle" ]]; then
    echo "Expected from demo/project.cmake:"
    echo "  - team=${expected_team:-<unset>}"
    echo "  - bundle=${expected_bundle:-<unset>}"
    echo
  fi
  if [[ -n "$identity_list" ]]; then
    echo "Available code-signing identities:"
    printf '%s\n' "$identity_list"
    echo
  fi
  if (( ${#profile_summaries[@]} > 0 )); then
    echo "Installed provisioning profiles:"
    printf '%s\n' "${profile_summaries[@]}"
    echo
  fi
  cat <<'EOF'
Open Xcode > Settings > Accounts, sign in to your Apple Developer account, and install:
  - an Apple Development certificate for iOS
  - an unexpired iOS provisioning profile that matches the bundle identifier above
EOF
  exit 1
fi

echo 'iOS signing preflight passed.'