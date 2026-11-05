#!/usr/bin/env bash
# Creates EasyNotepad's Android release key: a self-signed certificate in a keystore of its own
# (every Android signing key is self-signed; what matters is that every update is signed with the
# same key). Prints what to store as GitHub Actions secrets, so CI signs the release APK with it.
#
#   scripts/android-release-key.sh [keystore-file]     (default: ./easynotepad-release.jks)
#
# Needs keytool (any JDK). Keep the keystore and its passwords safe and out of the repository:
# an app signed with a lost key can never be updated again.
set -euo pipefail

KEYSTORE="${1:-easynotepad-release.jks}"
ALIAS="easynotepad"

command -v keytool > /dev/null || { echo "keytool not found: install a JDK (17 or newer)" >&2; exit 1; }
[ -e "$KEYSTORE" ] && { echo "$KEYSTORE exists already: not overwritten" >&2; exit 1; }
case "$(cd "$(dirname "$KEYSTORE")" && pwd)" in
    "$(git rev-parse --show-toplevel 2> /dev/null || echo /nonexistent)"*)
        echo "warning: $KEYSTORE is inside the repository: move it out before committing anything" >&2 ;;
esac

read -r -s -p "Keystore password (6 characters or more): " STORE_PASSWORD; echo
read -r -s -p "Again: " AGAIN; echo
[ "$STORE_PASSWORD" = "$AGAIN" ] || { echo "the passwords differ" >&2; exit 1; }
[ ${#STORE_PASSWORD} -ge 6 ] || { echo "too short" >&2; exit 1; }

# PKCS12 keystores have one password, for the store and the key.
keytool -genkeypair -v -storetype PKCS12 -keystore "$KEYSTORE" -alias "$ALIAS" \
    -keyalg RSA -keysize 4096 -validity 10000 \
    -storepass "$STORE_PASSWORD" -keypass "$STORE_PASSWORD" \
    -dname "CN=EasyNotepad, O=EasyNotepad"

cat <<EOF

Created $KEYSTORE (alias $ALIAS, valid 27 years). Back it up, with its password.

Add these repository secrets (GitHub: Settings > Secrets and variables > Actions):
  ANDROID_KEYSTORE_BASE64   the keystore, base64:  base64 -w0 "$KEYSTORE"   (macOS: base64 -i "$KEYSTORE")
  ANDROID_KEYSTORE_PASSWORD the password you typed
  ANDROID_KEY_ALIAS         $ALIAS
  ANDROID_KEY_PASSWORD      the same password

With them, CI builds and signs EasyNotepad-release.apk on every run and attaches it to releases.
Locally:
  cd android && gradle assembleRelease -PreleaseStoreFile="$(cd "$(dirname "$KEYSTORE")" && pwd)/$(basename "$KEYSTORE")" \\
      -PreleaseStorePassword=... -PreleaseKeyAlias=$ALIAS -PreleaseKeyPassword=...
EOF
