#!/bin/sh

for f in frontend/*.st; do
    ../bin/st_tester -set -f "$f" -cmd '../bin/storthc run -quiet $file'
done
../bin/st_tester -record
