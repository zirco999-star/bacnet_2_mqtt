#!/bin/bash

REP="${PWD##*/}"
echo "Processing $REP ..."
    
date_=$(date +"%Y%m%d%H%M")
npx repomix --style plain --include "*.ino" --include "src/*.{cpp,h,yaml,yml,json,py}" -o "/mnt/save/documentation/${REP}_${date_}.txt"
