#!/bin/sh
# Runner side: publish files as one generic package version of the repository's
# owner on Forgejo, under its version and again under "latest", replacing what was
# there. A "-<version>" in a file's name becomes "-latest" in the latest copy.
#
#   publish-package.sh <package> <version> <file>...
#
# Authenticates with PACKAGES_JWT, an Actions ID token for an Authorized Integration
# that carries write:package (the job's own token cannot write packages), or with
# PACKAGES_TOKEN, an access token. Needs the Forgejo variables of the job.
set -eu
pkg=$1; ver=$2; shift 2
[ $# -gt 0 ] || { echo "usage: $0 <package> <version> <file>..."; exit 2; }
owner=${GITHUB_REPOSITORY%%/*}
repo=${GITHUB_REPOSITORY#*/}
api="${GITHUB_SERVER_URL}/api/packages/${owner}/generic/${pkg}"
if [ -n "${PACKAGES_JWT:-}" ]; then
	auth="Authorization: bearer $PACKAGES_JWT"
else
	auth="Authorization: token ${PACKAGES_TOKEN:?no PACKAGES_JWT or PACKAGES_TOKEN to publish packages with}"
fi
resp=$(mktemp)
trap 'rm -f "$resp"' 0

for v in "$ver" latest; do
	# A version cannot be uploaded twice: remove the old one (404 when there is none).
	curl -s -m 60 -o /dev/null -X DELETE -H "$auth" "$api/$v" || true
	for file in "$@"; do
		name=$(basename "$file" | sed "s/-$ver/-$v/")
		code=$(curl -s -m 600 -o "$resp" -w '%{http_code}' -X PUT -H "$auth" --upload-file "$file" "$api/$v/$name")
		[ "$code" = 201 ] || {
			cat "$resp"; echo
			echo "publishing $name as $pkg $v: HTTP $code"
			exit 1
		}
		echo "$pkg $v: $name"
	done
done
# Attach the package to the repository (best effort: the page then lists it).
curl -s -m 60 -o /dev/null -X POST -H "$auth" \
	"${GITHUB_SERVER_URL}/api/v1/packages/${owner}/generic/${pkg}/-/link/${repo}" || true
echo "published $pkg $ver: ${GITHUB_SERVER_URL}/${owner}/-/packages/generic/${pkg}/${ver}"
