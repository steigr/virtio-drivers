#!/bin/sh
# Runner side: publish a file as a generic package of the repository's owner on
# Forgejo, under its version and again under "latest", replacing what was there.
#
#   publish-package.sh <package> <version> <file>
#
# Needs PACKAGES_TOKEN (a token that may write the owner's packages; the job's own
# token cannot) and the Forgejo variables of the job.
set -eu
pkg=$1; ver=$2; file=$3
owner=${GITHUB_REPOSITORY%%/*}
repo=${GITHUB_REPOSITORY#*/}
api="${GITHUB_SERVER_URL}/api/packages/${owner}/generic/${pkg}"
auth="Authorization: token ${PACKAGES_TOKEN:?no token to publish packages with}"
resp=$(mktemp)
trap 'rm -f "$resp"' 0

for v in "$ver" latest; do
	# A version cannot be uploaded twice: remove the old one (404 when there is none).
	curl -s -m 60 -o /dev/null -X DELETE -H "$auth" "$api/$v" || true
	name=$(basename "$file" | sed "s/-$ver/-$v/")
	code=$(curl -s -m 600 -o "$resp" -w '%{http_code}' -X PUT -H "$auth" --upload-file "$file" "$api/$v/$name")
	[ "$code" = 201 ] || {
		cat "$resp"; echo
		echo "publishing $name as $pkg $v: HTTP $code"
		exit 1
	}
done
# Attach the package to the repository (best effort: the page then lists it).
curl -s -m 60 -o /dev/null -X POST -H "$auth" \
	"${GITHUB_SERVER_URL}/api/v1/packages/${owner}/generic/${pkg}/-/link/${repo}" || true
echo "published $pkg $ver: ${GITHUB_SERVER_URL}/${owner}/-/packages/generic/${pkg}/${ver}"
