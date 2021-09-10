#!/bin/sh

for c in $(sed -n 's/^\t"\(.*\)": {/\1/p' doc/schemas/listconfigs.schema.json | grep -v '^# version$$' | grep -v '^plugins$$' | grep -v '^important-plugins$$')
do
  if
	  ! grep -q "^\s\+\*\*$c\*\*" doc/lightningd-config.5.md;
then
  echo "$c undocumented!"
  exit 1
fi
done

for c in $(grep -v '\[plugin ' doc/lightningd-config.5.md \
		| sed -n 's/^ \*\*\([^*]*\)\*\*.*/\1/p' \
		| grep -v '^\(help\|version\|mainnet\|testnet\|signet\|plugin\|important-plugin\|plugin-dir\|clear-plugins\)$$')
do
if ! grep -q '^\t\+"$c"' doc/schemas/listconfigs.schema.json
then
  echo "$c documented but not in schema!"
  exit 1
fi
done
