header="\xF0\x00\x20\x29\x02\x18\x14\x7C\x01\x05"
footer="\xF7"
msg=$1

echo -ne $header$msg$footer > /dev/mk2-0

