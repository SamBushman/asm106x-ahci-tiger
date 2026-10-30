#!/bin/sh
C=$HOME/asm1062-kext/tools/asmctl; R=$HOME/asm1062-kext/pm_result.txt; : > $R
diskutil mount disk2s3 >/dev/null 2>&1
T="/Volumes/Test HD/asm1062-pm"; S=/tmp/asmpm; sudo rm -rf "$T" "/Volumes/Test HD/asm1062-pm" $S; mkdir -p "$T" $S
for i in 1 2 3 4 5; do
  sudo dd if=/dev/rdisk0 of=$S/p$i bs=1m skip=$((5000+i*100)) count=32 2>/dev/null; sudo chmod 644 $S/p$i
  cp $S/p$i "$T/a$i"; sync; sleep 1
  sudo $C sleep >/dev/null; sleep 2; sudo $C wake >/dev/null; sleep 3
  cp $S/p$i "$T/b$i"; sync
  s=$(md5 < $S/p$i); a=$(md5 < "$T/a$i"); b=$(md5 < "$T/b$i")
  echo "cycle $i: pre-sleep $([ $s = $a ] && echo OK || echo BAD), post-wake $([ $s = $b ] && echo OK || echo BAD)" >> $R
done
echo "PHASE2 sleep with background writer running" >> $R
sudo dd if=/dev/rdisk0 of=$S/big bs=1m skip=6000 count=192 2>/dev/null; sudo chmod 644 $S/big
( cp $S/big "$T/big1"; echo "writer exit $?" >> $R ) &
sleep 1; sudo $C sleep >/dev/null; sleep 1; sudo $C wake >/dev/null; sleep 4
wait
cp $S/big "$T/big2"; sync
echo "big1 $([ $(md5 < $S/big) = $(md5 < "$T/big1") ] && echo OK || echo DIFFERS-or-failed-as-expected) ; big2 $([ $(md5 < $S/big) = $(md5 < "$T/big2") ] && echo OK || echo BAD)" >> $R
echo DONE >> $R
