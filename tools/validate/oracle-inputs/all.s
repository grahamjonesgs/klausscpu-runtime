addr r1, r2, r3
subr r1, r2, r3
addw r1, r2, r3
subw r1, r2, r3
addiw r1, r2, -1
addiw r1, r2, 5
mulw r1, r2, r3
mulr r1, r2, r3
mulur r1, r2, r3
mulhr r1, r2, r3
mulhur r1, r2, r3
divr r1, r2, r3
divur r1, r2, r3
modr r1, r2, r3
modur r1, r2, r3
andr r1, r2, r3
orr r1, r2, r3
xorr r1, r2, r3
shlr r1, r2, r3
shrr r1, r2, r3
sarr r1, r2, r3
rolr r1, r2, r3
rorr r1, r2, r3
cmpeqr r1, r2, r3
cmpner r1, r2, r3
cmpltr r1, r2, r3
cmpler r1, r2, r3
cmpgtr r1, r2, r3
cmpger r1, r2, r3
cmpultr r1, r2, r3
cmpuler r1, r2, r3
cmpugtr r1, r2, r3
cmpuger r1, r2, r3
minr r1, r2, r3
maxr r1, r2, r3
minur r1, r2, r3
maxur r1, r2, r3
copy r1, r2
cmprr r1, r2
cmprrw r1, r2
cmprvw r1, -5
cmprvw r1, 5
memget64 r1, r2
memset64 r1, r2
memget8 r1, r2
memget16 r1, r2
memget32 r1, r2
memset8 r1, r2
memset16 r1, r2
memset32 r1, r2
incr r1, r2
decr r1, r2
negr r1, r2
absr r1, r2
shlr1 r1, r2
shrr1 r1, r2
notr r1, r2
popcnt r1, r2
clz r1, r2
ctz r1, r2
bitrev r1, r2
bswap r1, r2
sextb r1, r2
sexth r1, r2
zextb r1, r2
zexth r1, r2
sextw r1, r2
zextw r1, r2
push r1
pop r1
getsp r1
setsp r1
addsp -8
addsp 1000
enter 0
enter 4194303
leave
leaveret
callr r5
jmpr r5
setr r1, 0
setr r1, 127
setr r1, -128
setr r1, 128
setr r1, -129
setr r1, 2147483647
setr r1, -2147483648
setr64 r1, 1, 2
addv r1, r2, 255
addv r1, r2, 256
minusv r1, r2, 255
andv r1, r2, 255
orv r1, r2, 0
xorv r1, r2, 200
shlv r1, r2, 0
shlv r1, r2, 63
shrv r1, r2, 1
shrav r1, r2, 63
mulv r1, r2, -3
divv r1, r2, 7
modv r1, r2, 7
rolv r1, r2, 1
rolv r1, r2, 7
rorv r1, r2, 1
rorv r1, r2, 63
addi r1, r2, 127
addi r1, r2, -128
addi r1, r2, 128
cmprv r1, -128
cmprv r1, 127
cmprv r1, 128
ldidx64 r1, r2, 0
ldidx64 r1, r2, 1016
ldidx64 r1, r2, -1024
ldidx64 r1, r2, 1024
ldidx64 r1, r2, 4
stidx64 r1, r2, -8
ldidx32 r1, r2, 508
ldidx32 r1, r2, -512
stidx32 r1, r2, 2
ldidx16 r1, r2, 254
ldidx16 r1, r2, -256
stidx16 r1, r2, 1
ldidx8 r1, r2, 127
ldidx8 r1, r2, -128
ldidx8 r1, r2, 128
stidx8 r1, r2, -1
ldidx8s r1, r2, 3
ldidx16s r1, r2, 6
ldidx32s r1, r2, 12
ldidx32s r1, r2, 100000
jmp 0x1000
jmpz 0x1000
jmpnz 0x1000
jmpe 0x1000
jmpne 0x1000
jmpc 0x1000
jmpnc 0x1000
jmps 0x1000
jmpns 0x1000
jmplt 0x1000
jmple 0x1000
jmpgt 0x1000
jmpge 0x1000
jmpult 0x1000
jmpule 0x1000
jmpugt 0x1000
jmpuge 0x1000
jmpo 0x1000
jmpno 0x1000
call 0x1000
callz 0x1000
callnz 0x1000
calle 0x1000
callne 0x1000
callc 0x1000
callnc 0x1000
callo 0x1000
callno 0x1000
jmprel 64
jmpzrel 64
jmpnzrel 64
jmperel 64
jmpnerel 64
jmpcrel 64
jmpncrel 64
jmpsrel 64
jmpnsrel 64
jmpltrel 64
jmplerel 64
jmpgtrel 64
jmpgerel 64
jmpultrel 64
jmpulerel 64
jmpugtrel 64
jmpugerel 64
callrel 64
leapc r1, 64
ret
nop
halt
iret
trap
wait
addc r1, r2, r3
subc r1, r2, r3
bsetrr r1, r2, r3
bclrrr r1, r2, r3
btglrr r1, r2, r3
btstrr r1, r2, r3
setfr r1
shlar r1, r2
shrar r1, r2
rolr1 r1, r2
rorr1 r1, r2
rolcr r1, r2
rorcr r1, r2
bset r1, r2, 63
bclr r1, r2, 0
btgl r1, r2, 31
bextr r1, 0x0804
bdep r1, r1, r0, 0x0804
btst r1, 63
memreadrr r1, r2
memset64rr r1, r2
memreadr r1, 0x1000
memsetr r1, 0x1000
ldidx64u r1, r2, 8
stidx64u r1, r2, 8
ldidx64u r1, r2, 4
ldidx64r r1, r2, r3
stidx64r r1, r2, r3
pushv 5
pushv64 1, 2
delayr r1
lcddatar r1
delayv 100
lcd 1
lcdcmdv 1
lcddatav 1
fbr r1, r2, 0, 64
fbri r1, -8, 3, -64
fbri r1, 7, 12, 64
