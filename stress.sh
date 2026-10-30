#!/bin/sh
T="/Volumes/Test HD/asm1062-stress"; S=/tmp/asmstress; R=$HOME/asm1062-kext/stress_result.txt
sudo rm -rf "$T" $S; mkdir -p $S "$T"; : > $R
# writer sources (from SSD)
for w in 1 2; do sudo dd if=/dev/rdisk0 of=$S/w$w bs=1m skip=$((4000+w*400)) count=256 2>/dev/null; done
sudo chmod 644 $S/w*; ( cd $S && md5 w1 w2 ) > $S/w.md5
# readers: 4 regions x 3 passes, background
for r in 1 2 3 4; do
  ( for pass in 1 2 3; do sudo dd if=/dev/rdisk2 bs=64k count=8192 skip=$((r*40000)) 2>/dev/null | md5; done | sort | uniq -c > $S/r$r.out ) &
done
# writers in parallel with readers
( cp $S/w1 "$T/w1"; sync ) &
( cp $S/w2 "$T/w2"; sync ) &
wait
for r in 1 2 3 4; do echo "reader $r: $(cat $S/r$r.out | tr '\n' ' ')" >> $R; done
( cd "$T" && md5 w1 w2 ) > $S/w.cached.md5; diff $S/w.md5 $S/w.cached.md5 >> $R && echo "writers cached md5 MATCH" >> $R
echo DONE >> $R
