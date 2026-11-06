#!/bin/sh
# Vendored from update publisher/publisher (scripts/publisher-client.sh).
# Publishes a release through the appcast-publisher, from a Forgejo Actions job:
#
#   publisher-client.sh <app> <version> <release> <archive> [<asset>...]
#
# <version> is the app's CFBundleVersion (sparkle:version), <release> the release's directory on the landing pages
# ("2.9.2.143", "0.9.0-dev.57"), <archive> what Sparkle downloads, and the assets more files for the same directory
# (disk image, checksums, provenance). The job needs `enable-openid-connect: true`: the request carries an ID token
# for the publisher's audience, and the publisher's policy for this repository, ref and workflow decides whether it
# may publish this app on this channel. Nothing secret is passed to this script.
#
#   SHORT_VERSION           CFBundleShortVersionString (sparkle:shortVersionString)
#   CHANNEL                 Sparkle channel, empty for the default one
#   CHANGELOG               a Keep a Changelog file; its entry for the release becomes the release notes
#   MINIMUM_SYSTEM_VERSION  sparkle:minimumSystemVersion
#   APPCAST                 "false" for an app Sparkle does not update (an IRIX inst package): only the files are
#                           published, with no appcast item and no signature
#   PUBLISHER_URL           the service (default $PUBLISHER_URL)
#   PUBLISHER_AUDIENCE      the audience it accepts (default publisher)
#
# A release whose version is already in the appcast counts as published, so a re-run passes.
set -eu
app=$1
version=$2
release=$3
archive=$4
shift 4
url=${PUBLISHER_URL:-$PUBLISHER_URL}
audience=${PUBLISHER_AUDIENCE:-publisher}

: "${ACTIONS_ID_TOKEN_REQUEST_URL:?the workflow needs enable-openid-connect: true}"
: "${ACTIONS_ID_TOKEN_REQUEST_TOKEN:?the workflow needs enable-openid-connect: true}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A curl config file keeps file names with spaces intact without arrays.
form() { printf 'form = "%s"\n' "$1" >>"$tmp/form"; }
form "version=$version"
form "release=$release"
[ -z "${SHORT_VERSION:-}" ] || form "shortVersion=$SHORT_VERSION"
[ -z "${CHANNEL:-}" ] || form "channel=$CHANNEL"
[ -z "${MINIMUM_SYSTEM_VERSION:-}" ] || form "minimumSystemVersion=$MINIMUM_SYSTEM_VERSION"
[ -z "${CHANGELOG:-}" ] || form "changelog=@$CHANGELOG"
[ -z "${APPCAST:-}" ] || form "appcast=$APPCAST"
form "file=@$archive"
for asset in "$@"; do
	form "asset=@$asset"
done

echo "publisher: $app $version as $release${CHANNEL:+ on $CHANNEL}: $(basename "$archive") $*"
for attempt in 1 2 3 4 5; do
	token=$(curl -fsS -H "Authorization: bearer $ACTIONS_ID_TOKEN_REQUEST_TOKEN" \
		"$ACTIONS_ID_TOKEN_REQUEST_URL&audience=$audience" | jq -r .value)
	code=$(curl -sS -o "$tmp/response.json" -w '%{http_code}' -K "$tmp/form" \
		-H "Authorization: Bearer $token" "$url/apps/$app/releases")
	case $code in
	201)
		jq . "$tmp/response.json"
		exit 0
		;;
	409)
		echo "publisher: $app $version is already published"
		exit 0
		;;
	503)
		echo "publisher: busy, retrying ($attempt)"
		sleep $((attempt * 5))
		;;
	*)
		echo "publisher said $code:" >&2
		head -c 2000 "$tmp/response.json" >&2
		echo >&2
		exit 1
		;;
	esac
done
echo "publisher: gave up" >&2
exit 1
