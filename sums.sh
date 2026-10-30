#!/bin/sh
# Raw checksum set for /dev/rdisk2 (Test HD drive). Re-run with the same offsets via the new driver and diff.
DEV=${DEV:-/dev/rdisk2}; TOTAL=1465149168; N=400; CNT=8192
OUT=$1
: > "$OUT"
sum() { echo "$1 $2 $(dd if=$DEV bs=512 skip=$1 count=$2 2>/dev/null | md5)" >> "$OUT"; }
sum 0 65536
sum $((TOTAL-65536)) 65536
sum $((268435456-4096)) $CNT          # straddles 2^28 sectors (128 GiB): LBA28/LBA48 boundary
sum 262208 $CNT                       # start of Test HD HFS+ (volume header at +2 sectors)
sum 1460954848 $CNT                   # start of ThinkTanks
i=0
while [ $i -lt $N ]; do
  sum $(( i * (TOTAL-CNT) / (N-1) )) $CNT
  i=$((i+1))
done
echo "slice /dev/rdisk2s5 $(dd if=/dev/rdisk2s5 bs=1m 2>/dev/null | md5)" >> "$OUT"
echo DONE >> "$OUT"
