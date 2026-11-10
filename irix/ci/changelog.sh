#!/bin/sh
# Writes an inst package's changelog for the appcast-publisher (publisher-client.sh's CHANGELOG) to stdout: the
# changelog published for the package, with an entry for this build on top. The IRIX landing page shows it under What's new.
# The same script is in each irix/* repository that publishes packages.
#
#   changelog.sh <package> <version> <checkout> [<upstream> <notes-url>]
#
# The entry names the upstream release a package is built from ("OpenSSL 3.6.0", with a link to its release notes),
# lists the commits of <checkout> since the one the previous entry was built from (it needs their history: no
# shallow clone), and links the commit it is built from. When the published changelog has an entry for <version>
# already, it is written as it is.
#
#   UPDATES_URL  where the bucket is served (required; the organisation variable of the same name)
#   CHANGELOG_PATHS  space-separated paths the listed commits must touch (default the whole checkout)
#
# When the published changelog can't be read, it fails and writes nothing: the release is then published without a
# changelog, which leaves the published one as it is, rather than replacing it with this entry alone.
set -eu
package=$1
version=$2
checkout=$3
upstream=${4:-}
notes=${5:-}
base=${UPDATES_URL:?where the update publisher serves packages}
commits=${GITHUB_SERVER_URL:?}/${GITHUB_REPOSITORY:?}/commit

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

code=$(curl -sS -o "$tmp/published" -w '%{http_code}' "$base/$package/CHANGELOG.md")
case $code in
200) ;;
404) : >"$tmp/published" ;;
*)
	echo "changelog: $base/$package/CHANGELOG.md answered $code" >&2
	exit 1
	;;
esac

if awk -v v="## [$version]" 'index($0, v) == 1 { found = 1 } END { exit !found }' "$tmp/published"; then
	cat "$tmp/published"
	exit 0
fi

head=$(git -C "$checkout" rev-parse HEAD)
# The newest entry comes first: its commit is the first link to one.
previous=$(grep -o "$commits/[0-9a-f]\{40\}" "$tmp/published" | head -n 1 | sed 's|.*/||')

printf '# Changelog\n\nThe %s package for IRIX 6.5.\n\n' "$package"
printf '## [%s] - %s\n\n' "$version" "$(date -u +%Y-%m-%d)"
[ -z "$upstream" ] || printf '%s\n' "- $upstream $version ([release notes]($notes))."
if [ -n "$previous" ] && git -C "$checkout" merge-base --is-ancestor "$previous" "$head" 2>/dev/null; then
	git -C "$checkout" log --no-merges --format='- %s' "$previous..$head" -- ${CHANGELOG_PATHS:-.} | sed 's/ *\[skip ci\]//'
fi
printf '%s\n' "- Built from [$(echo "$head" | cut -c1-7)]($commits/$head)."
awk '/^## / && !seen { print ""; seen = 1 } seen' "$tmp/published"
