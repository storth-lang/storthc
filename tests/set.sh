#!/bin/sh

cd "$(dirname "$0")" || exit 1

TESTER=../build/st_tester
CMD='../build/storthc run $file --quiet --import_dir .. -o build/$file'

mkdir -p build/
echo "*" > build/.gitignore

if [ ! -x "$TESTER" ] || [ ! -x ../build/storthc ]; then
    make -C .. build/storthc build/st_tester || exit 1
fi

if [ "$1" = record ] && [ -f record.sth ]; then
    exec "$TESTER" -record
fi

if [ "$1" = all ] || [ ! -f record.sth ]; then
    files=$(ls *.st)
elif [ $# -eq 0 ]; then
    echo "record.sth already exists: pass the .st files to add, or 'all' to re-snapshot everything"
    exit 1
else
    files="$*"
fi

"$TESTER" -set -cmd "$CMD" -f $files || exit 1
"$TESTER" -record || exit 1
"$TESTER"
