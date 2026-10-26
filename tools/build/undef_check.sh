#!/bin/bash
# undef_check.sh: list every symbol the linked title still needs that no SDK stub exports (the converter only reports the first one).
# usage: bash undef_check.sh        (after a failed make_jvm_title.sh; reads ~/ps5probe/build/llvm-pie.elf)
S=${PS5_SDK:-$HOME/ps5probe/.deps/native/ps5-payload-sdk}
ELF=$HOME/ps5probe/build/llvm-pie.elf
[ -f $ELF ] || { echo "no $ELF"; exit 1; }
"$S/bin/llvm-nm" -u "$ELF" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//' | sort -u > /tmp/undef.txt
for f in "$S"/target/lib/*; do
  [ -f "$f" ] || continue
  "$S/bin/llvm-nm" -D --defined-only "$f" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//'
  "$S/bin/llvm-nm" --defined-only "$f" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//'
done | sort -u > /tmp/stubexports.txt
echo "undefined in ELF: $(wc -l < /tmp/undef.txt), exported by SDK stubs: $(wc -l < /tmp/stubexports.txt)"
echo "== needed but not exported by any stub:"
comm -23 /tmp/undef.txt /tmp/stubexports.txt | grep -v '^$' | head -80
