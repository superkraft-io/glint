#!/bin/zsh
set -euo pipefail

print_section() {
  local title="$1"
  echo
  echo "$title"
  printf '%*s\n' "${#title}" '' | tr ' ' '-'
}

extract_team_ids_from_identities() {
  security find-identity -v -p codesigning 2>/dev/null \
    | sed -n 's/.*(\([A-Z0-9]\{10\}\)).*/\1/p' \
    | sort -u
}

extract_team_ids_from_profiles() {
  local profiles_dir="$HOME/Library/MobileDevice/Provisioning Profiles"
  if [[ ! -d "$profiles_dir" ]]; then
    return 0
  fi

  grep -aRho 'TeamIdentifier[^[:cntrl:]]*[A-Z0-9]\{10\}' "$profiles_dir" 2>/dev/null \
    | sed -n 's/.*\([A-Z0-9]\{10\}\)$/\1/p' \
    | sort -u
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