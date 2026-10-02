#!/bin/zsh
set -euo pipefail

print_section() {
  local title="$1"
  echo
  echo "$title"
  printf '%*s\n' "${#title}" '' | tr ' ' '-'
}

# A certificate's team is its Organizational Unit (OU).  The 10-character ID in
# parentheses at the end of an identity's name is the team only for
# distribution certificates; for "Apple Development" / "iPhone Developer" ones
# it identifies the developer.
extract_team_ids_from_identities() {
  local identity
  security find-identity -v -p codesigning 2>/dev/null \
    | sed -n 's/^ *[0-9][0-9]*) [0-9A-F]* "\(.*\)"$/\1/p' \
    | sort -u \
    | while IFS= read -r identity; do
        security find-certificate -a -c "$identity" -p 2>/dev/null \
          | openssl x509 -noout -subject 2>/dev/null \
          | sed -n 's/.*OU *= *\([A-Z0-9]\{10\}\).*/\1/p' || true
      done \
    | sort -u
}

# Profiles are signed plists with the team under TeamIdentifier, on its own
# line: decode them instead of grepping the raw files.
extract_team_ids_from_profiles() {
  local profiles_dir="$HOME/Library/MobileDevice/Provisioning Profiles"
  if [[ ! -d "$profiles_dir" ]]; then
    return 0
  fi

  local decoded profile
  decoded="$(mktemp)"
  for profile in "$profiles_dir"/*.mobileprovision(.N) "$profiles_dir"/*.provisionprofile(.N); do
    security cms -D -i "$profile" > "$decoded" 2>/dev/null || continue
    /usr/libexec/PlistBuddy -c 'Print :TeamIdentifier:0' "$decoded" 2>/dev/null || true
  done | sort -u
  rm -f "$decoded"
}

identity_ids="$(extract_team_ids_from_identities || true)"
profile_ids="$(extract_team_ids_from_profiles || true)"
all_ids="$(printf '%s\n%s\n' "$identity_ids" "$profile_ids" | sed '/^$/d' | sort -u)"

print_section "Apple Team IDs"

if [[ -z "$all_ids" ]]; then
  echo "No Apple development team IDs were found on this Mac."
  echo "Sign into Xcode with an Apple Developer account first, then rerun this script."
  exit 1
fi

echo "$all_ids"

print_section "Suggested demo/project.cmake"

first_id="$(echo "$all_ids" | head -n 1)"
cat <<EOF
set(GLINT_IOS_DEVELOPMENT_TEAM "$first_id" CACHE STRING "" FORCE)
set(GLINT_IOS_BUNDLE_IDENTIFIER "io.superkraft.glintdemo" CACHE STRING "" FORCE)
EOF

print_section "Raw sources"

echo "Codesigning identities:"
if [[ -n "$identity_ids" ]]; then
  echo "$identity_ids"
else
  echo "  none found"
fi

echo

echo "Provisioning profiles:"
if [[ -n "$profile_ids" ]]; then
  echo "$profile_ids"
else
  echo "  none found"
fi

echo

echo "If multiple IDs are listed, use the one that matches the Apple account/team you want to sign with in Xcode."