#!/bin/sh
# write test: sources come straight off the SSD (disk0) so they are independent, real, non-compressible data
T="/Volumes/Test HD/asm1062-write-test"; S=/tmp/asmsrc; R=$HOME/asm1062-kext/wtest_result.txt
sudo rm -rf "$T" $S; mkdir -p $S "$T"; : > $R
i=0
for sz in 1024 4096 65536 524288 524289 1048576 16777216 134217728 536870912; do
  i=$((i+1)); skip=$((2000000+i*500000))
  n=$(( (sz+511)/512 ))
  sudo dd if=/dev/rdisk0 of=$S/f$sz bs=512 skip=$skip count=$n 2>/dev/null
  sudo dd if=$S/f$sz of=$S/g$sz bs=1 count=0 2>/dev/null
  sudo sh -c "dd if=$S/f$sz of=$S/h$sz bs=1m 2>/dev/null; true"
  # trim to exact size (handles 524289 etc.) without pipes
  sudo perl -e 'truncate($ARGV[0], $ARGV[1]) or die "truncate: $!"' $S/f$sz $sz
  sudo rm -f $S/g$sz $S/h$sz
  actual=$(stat -f %z $S/f$sz); [ "$actual" = "$sz" ] || echo "SOURCE SIZE BAD f$sz = $actual" >> $R
done
sudo chmod 644 $S/f*; ls -l $S >> $R
( cd $S && md5 f* ) > $S/src.md5
s=$(date +%s); cp $S/f* "$T/"; sync; e=$(date +%s); echo "copy+sync of all files: $((e-s))s" >> $R
( cd "$T" && md5 f* ) > $S/cached.md5
diff $S/src.md5 $S/cached.md5 >> $R && echo "cached md5: MATCH ($(wc -l < $S/src.md5) files)" >> $R
echo WRITTEN >> $R
