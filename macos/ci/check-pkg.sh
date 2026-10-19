#!/bin/sh
# In the Panther guest: check-pkg.sh <dir on the share holding <PKG>-*.pkg.tar.gz and component.sh>
# Each package's BOM and archive must list CHECK_PATHS (component.sh), and the BOM must read back with
# the guest's own lsbom. Installing is not done here: the guest is the runner's live build node.
set -e
dir=$1
. "$dir/component.sh"
w=/tmp/checkpkg-$$
rm -rf "$w"; mkdir -p "$w"
cd "$w"
status=0
for f in "$dir"/$PKG-*.pkg.tar.gz; do
  n=`basename "$f" .pkg.tar.gz`
  mkdir "$n"
  gzip -dc "$f" | (cd "$n" && tar xf -)
  bom="$n/$PKG.pkg/Contents/Archive.bom"
  lsbom -s "$bom" > "$n.bom.txt" || { echo "$n: lsbom cannot read the BOM"; status=1; continue; }
  gzip -dc "$n/$PKG.pkg/Contents/Archive.pax.gz" | /bin/pax > "$n.pax.txt" || { echo "$n: pax cannot read the archive"; status=1; continue; }
  for want in $CHECK_PATHS; do
    grep -q -F "$want" "$n.bom.txt" && grep -q -F "$want" "$n.pax.txt" || { echo "$n: $want is not in both"; status=1; }
  done
  echo "$n: BOM $(wc -l < "$n.bom.txt" | tr -d ' ') entries, archive $(wc -l < "$n.pax.txt" | tr -d ' ')"
done
cd /
rm -rf "$w"
exit $status
