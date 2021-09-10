#!/bin/sh

sed -n 's/^\t"\(.*\)": {/\1/p' doc/schemas/listconfigs.schema.json \
  | grep -v '^# version$$' | grep -v '^plugins$$' \
  | grep -v '^important-plugins$$'
