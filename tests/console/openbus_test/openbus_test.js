; -*-asm-*-
;;; openbus_test - what does a DSP store into DRAM write when the DSP loses
;;; the bus the cycle after it starts?
;;;
;;; Atari's technote JDEV0002 (GPU/DSP bugs, item 7): "The DSP must not do an
;;; external write unless it is preceded by an external read that will
;;; complete for the write starts." The netlist (JagMCP --open-bus,
;;; Jaguar_MiSTer) writes $FFFF in the high half and the bus's last half word
;;; in the low half; the console a long word's high half twice.
;;;
;;; 1. JagMCP's references random_external_0110 and _0195 (DSP programs at
;;;    $F1B000, started as the simulation starts them). Each runs RUNS_REF times, each time after a different delay; $1000-$3FFF is
;;;    compared with this program's run in JagMCP without the open bus
;;;    (exp*.bin, "make expected": its last run of each is kept at SAVE).
;;;    0195 stores a DIV result at $1808: through the BIOS (its MEMCON)
;;;    JagMCP gives $0007F000 there, the simulation's boot $00000000.
;;; 2. The DSP stores N_ST longs, words or bytes into DRAM (a value made from
;;;    the address, never all ones), without a load before them, with the
;;;    Blitter copying in DRAM or the bus otherwise quiet; longs also with a
;;;    load before each store (the technote's way). RUNS_ST runs each.
;;; Shown per test: wrong runs (1) or wrong stores (2); of these, all ones
;;; ($FFFF in a long's high half, a word or a byte $FF..), and the pattern
;;; (a long: its high half twice; a word or a byte: the one stored before);
;;; second line: the first wrong one (address, value) and, for 1, the key
;;; long of the last run.

	gpu
	include <js/symbols/jagregeq.js>
	include <js/symbols/blit_eq.js>
	include <js/macro/help.mac>
	include <js/macro/module.mac>

	unreg	LR,LR.a,SP,SP.a
LR		reg 29
SP		reg 28

VID_MODE	equ $4c1		; CRY16|VIDEN|PWIDTH3|BGEN|CSYNC
BG_COLOR	equ $2420
TXT_FG		equ $78ff
max_x_txt	equ 384
max_y_txt	equ 8*28
TxtScreen	equ $140000
op_list		equ $800
stacktop	equ $f04000

REF_LO		equ $1000		; what the references use, compared
REF_LEN		equ $3000
RUNS_REF	equ 256
REF_WAIT	equ 2000		; GPU loops before polling D_CTRL
SAVE		equ $40000		; the last run of each reference, for exp*.bin

STBUF		equ $100000		; the DSP's stores
N_ST		equ 2048
KEY		equ $5a5a0000		; a long: address ^ KEY
WMASK		equ $3fff		; a word: (address >> 1) & WMASK | WBIT
WBIT		equ $4000
BMASK		equ $3f			; a byte: address & BMASK | BBIT
BBIT		equ $40
RUNS_ST		equ 64
ST_WAIT		equ 20000
BLITSRC		equ $180000		; the Blitter's copy meanwhile
BLITDST		equ $1a0000
BLITPIX		equ 32760		; 16-bit pixels, ~90k cycles

	RSSET $30000
	include <js/var/txtscr.var>
	RSL	Results,6*9		; per test: wrong, ones, pattern, first address, value, key
	global	Results

	run $4000
start::
	movei	#init,r0
	nop
	jump	(r0)
	nop
	dc.l	$4e722700

;;; ----------------------------------------
;;; runDsp, in GPU RAM: off the bus while the DSP runs
;;; r0 - delay loops before the start (>= 1)
;;; r1 - 1: the Blitter copies BLITSRC to BLITDST meanwhile
;;; r2 - loops before D_CTRL is polled (>= 1)
;;; starts the DSP at D_RAM with D_FLAGS = REGPAGE (as the simulation), returns
;;; when it has stopped and the Blitter is done. r0..r4, r14, r15 destroyed
	MODULE runner,$f03000
runDsp::
	movei	#BLIT_A1_BASE,r14
	movei	#.delay,r3
	cmpq	#0,r1
	movei	#$f1a100,r15		; D_FLAGS
	jump	eq,(r3)
	nop
	movei	#BLITDST,r3
	movei	#BLITSRC,r4
	moveq	#0,r1
	store	r3,(r14)		; A1_BASE
	store	r4,(r14+_BLIT_A2_BASE)
	store	r1,(r14+_BLIT_A1_PIXEL)
	store	r1,(r14+_BLIT_A2_PIXEL)
	movei	#1<<16|BLITPIX,r3
	movei	#BLIT_SRCEN|BLIT_LFU_REPLACE,r4
	store	r3,(r14+_BLIT_COUNT)
	store	r4,(r14+_BLIT_CMD)
.delay
	subq	#1,r0
	jr	ne,.delay
	nop
	movei	#$4000,r3
	movei	#DSP_RAM,r4
	store	r3,(r15)		; D_FLAGS: REGPAGE
	moveq	#1,r3
	store	r4,(r15+$10)		; D_PC
	store	r3,(r15+$14)		; D_CTRL: go
.wait
	subq	#1,r2
	jr	ne,.wait
	nop
.dsp
	load	(r15+$14),r3
	btst	#0,r3
	jr	ne,.dsp
	nop
.blit
	load	(r14+_BLIT_CMD),r3
	btst	#0,r3			; 1: done
	jr	eq,.blit
	nop
	jump	(LR)
	nop
	ENDMODULE runner

;;; ----------------------------------------
;;; the DSP's stores: N_ST from STBUF, the value made from the address
	MACRO DSTOP
	movei	#$f1a114,r30
	moveq	#0,r31
.\s	store	r31,(r30)
	jr	.\s
	nop
	ENDM

	dsp
	MODULE dst_l,$f1b000		; longs, no load before them
	movei	#STBUF,r0
	movei	#N_ST,r1
	movei	#KEY,r3
.l	move	r0,r2
	xor	r3,r2
	store	r2,(r0)
	addqt	#4,r0
	subq	#1,r1
	jr	ne,.l
	nop
	DSTOP
	ENDMODULE dst_l

	MODULE dst_lr,$f1b000		; longs, a load before each
	movei	#STBUF,r0
	movei	#N_ST,r1
	movei	#KEY,r3
	movei	#STBUF-8,r5
.l	load	(r5),r6
	move	r0,r2
	or	r6,r6			; the load is done
	xor	r3,r2
	store	r2,(r0)
	addqt	#4,r0
	subq	#1,r1
	jr	ne,.l
	nop
	DSTOP
	ENDMODULE dst_lr

	MODULE dst_w,$f1b000		; words
	movei	#STBUF,r0
	movei	#N_ST,r1
	movei	#WMASK,r3
	movei	#WBIT,r4
.l	move	r0,r2
	shrq	#1,r2
	and	r3,r2
	or	r4,r2
	storew	r2,(r0)
	addqt	#2,r0
	subq	#1,r1
	jr	ne,.l
	nop
	DSTOP
	ENDMODULE dst_w

	MODULE dst_b,$f1b000		; bytes
	movei	#STBUF,r0
	movei	#N_ST,r1
	movei	#BMASK,r3
	movei	#BBIT,r4
.l	move	r0,r2
	and	r3,r2
	or	r4,r2
	storeb	r2,(r0)
	addqt	#1,r0
	subq	#1,r1
	jr	ne,.l
	nop
	DSTOP
	ENDMODULE dst_b
	gpu

;;; ----------------------------------------
	align	4
init::
	movei	#$f02100,r0
	movei	#1<<14|%11111<<9,r1	; all interrupts off and cleared, bank 1
	store	r1,(r0)
	nop
	nop
	movei	#stacktop,SP
	INITMODULE runner

	include <js/inc/videoinit.inc>
	movei	#$f00028,r0
	movei	#VID_MODE,r1
	storew	r1,(r0)
	movei	#$f00058,r0
	movei	#BG_COLOR,r1
	storew	r1,(r0)
	movei	#$f14000,r0		; JOYSTICK as the simulation's boot program
	movei	#$0100,r1
	storew	r1,(r0)
;;; Blitter: 16-bit pixels, phrase mode
	movei	#BLIT_A1_BASE,r14
	movei	#BLIT_PITCH1|BLIT_PIXEL16|BLIT_WID3584|BLIT_XADDPHR,r1
	store	r1,(r14+_BLIT_A1_FLAGS)
	store	r1,(r14+_BLIT_A2_FLAGS)
;;; text
	movei	#254,r0
	movei	#BG_COLOR<<16|TXT_FG,r1
	movei	#TxtScreen,r2
	movei	#ASCII,r3
	movei	#InitTxtScreen,r4
	BL	(r4)
;;; object list
	movei	#obl,r1
	movei	#op_list,r2
	movei	#(obl_end-obl)/4,r3
.cpy	load	(r1),r0
	addqt	#4,r1
	subq	#1,r3
	store	r0,(r2)
	jr	ne,.cpy
	addqt	#4,r2
	movei	#$f00020,r0
	movei	#op_list,r1
	rorq	#16,r1
	store	r1,(r0)

	movei	#title,r0
	moveq	#0,r1
	movei	#PrintString_YX,r4
	BL	(r4)
	movei	#header,r0
	movei	#1<<16,r1
	movei	#PrintString_YX,r4
	BL	(r4)
	movei	#Results,r9
	movei	#3<<16,r24		; text line << 16
	movei	#SAVE,r10
;;; 1. the references
	movei	#tab_ref,r11
.ref	load	(r11),r20		; program
	addqt	#4,r11
	cmpq	#0,r20
	movei	#.refs_done,r0
	jump	eq,(r0)
	nop
	load	(r11),r21		; its length
	addqt	#4,r11
	load	(r11),r22		; expected
	addqt	#4,r11
	load	(r11),r23		; key long
	addqt	#4,r11
	load	(r11),r0		; name
	addqt	#4,r11
	movei	#testRef,r1
	BL	(r1)
	movei	#.ref,r0
	jump	(r0)
	nop
.refs_done
	movei	#1<<16,r0		; a gap
	add	r0,r24
;;; 2. the DSP's stores
	movei	#tab_st,r11
.st	load	(r11),r20		; program
	addqt	#4,r11
	cmpq	#0,r20
	movei	#.st_done,r0
	jump	eq,(r0)
	nop
	load	(r11),r21		; its length
	addqt	#4,r11
	load	(r11),r22		; 1: Blitter
	addqt	#4,r11
	load	(r11),r23		; size: 4, 2, 1
	addqt	#4,r11
	load	(r11),r0		; name
	addqt	#4,r11
	movei	#testStore,r1
	BL	(r1)
	movei	#.st,r0
	jump	(r0)
	nop
.st_done
	movei	#done_txt,r0
	move	r24,r1
	movei	#PrintString_YX,r4
	BL	(r4)

;;; restore the object list each frame before the text (the OP writes
;;; back HEIGHT and DATA of the bitmap)
.done
	movei	#$f00006,r10
	movei	#2*44-8,r11		; a few lines before the text (obl.S: y_txt)
.vc	loadw	(r10),r0
	bclr	#11,r0
	cmp	r11,r0
	jr	ne,.vc
	nop
	movei	#obl,r1
	movei	#op_list,r2
	movei	#(obl_end-obl)/4,r3
.cp	load	(r1),r0
	addqt	#4,r1
	subq	#1,r3
	store	r0,(r2)
	jr	ne,.cp
	addqt	#4,r2
	movei	#.done,r0
	jump	(r0)
	nop

;;; ----------------------------------------
;;; testRef - runs a reference RUNS_REF times and compares REF_LO..
;;; r0 - name, r20 - program, r21 - its length (bytes), r22 - expected
;;; memory, r23 - key long, r24 - text line << 16 (advanced), r9 - results
;;; (advanced), r10 - where the last run is kept (advanced)
testRef::
	move	LR,r27
	move	r24,r1
	movei	#PrintString_YX,r4
	BL	(r4)
	moveq	#0,r16			; run
	moveq	#0,r17			; first wrong address
	moveq	#0,r18			; its value
	moveq	#0,r25			; wrong runs
	moveq	#0,r26			; runs with $FFFF in the key long's high half
	moveq	#0,r8			; runs with its high half twice
.run
	;; REF_LO..: 0, then the simulation's pokes
	movei	#REF_LO,r0
	movei	#REF_LEN/4,r1
	moveq	#0,r2
.clr	store	r2,(r0)
	subq	#1,r1
	jr	ne,.clr
	addqt	#4,r0
	movei	#pokes,r0
	moveq	#6,r1
.pk	load	(r0),r2
	addqt	#4,r0
	load	(r0),r3
	addqt	#4,r0
	subq	#1,r1
	jr	ne,.pk
	store	r3,(r2)
	;; the program into the DSP's RAM
	move	r20,r0
	movei	#DSP_RAM,r1
	move	r21,r2
.cp	load	(r0),r3
	addqt	#4,r0
	subq	#4,r2
	store	r3,(r1)
	jr	ne,.cp
	addqt	#4,r1
	;; run after 7*run+1 delay loops
	move	r16,r0
	shlq	#3,r0
	sub	r16,r0
	addq	#1,r0
	moveq	#0,r1
	movei	#REF_WAIT,r2
	movei	#runDsp,r3
	BL	(r3)
	;; compare
	movei	#REF_LO,r0
	move	r22,r1
	movei	#REF_LEN/4,r2
	moveq	#0,r3			; wrong longs in this run
.cmp	load	(r0),r4
	load	(r1),r5
	addqt	#4,r1
	cmp	r4,r5
	jr	eq,.same
	nop
	addq	#1,r3
	cmpq	#0,r17
	jr	ne,.same
	nop
	move	r0,r17
	move	r4,r18
.same	subq	#1,r2
	jr	ne,.cmp
	addqt	#4,r0
	cmpq	#0,r3
	jr	eq,.right
	nop
	addq	#1,r25
.right	load	(r23),r19		; the key long
	movei	#.notwice,r6
	cmpq	#0,r3
	jump	eq,(r6)			; a right run: not counted
	nop
	movei	#$ffff,r5
	move	r19,r4
	shrq	#16,r4
	cmp	r4,r5
	jr	ne,.noff
	nop
	addq	#1,r26
.noff	move	r19,r5
	shlq	#16,r5
	shrq	#16,r5
	cmp	r4,r5			; high half = low half
	jr	ne,.notwice
	nop
	addq	#1,r8
.notwice
	addq	#1,r16
	movei	#RUNS_REF,r0
	movei	#.run,r1
	cmp	r0,r16
	jump	ne,(r1)
	nop
	;; the last run kept for exp*.bin
	movei	#REF_LO,r0
	movei	#REF_LEN/4,r2
.keep	load	(r0),r3
	addqt	#4,r0
	subq	#1,r2
	store	r3,(r10)
	jr	ne,.keep
	addqt	#4,r10
	movei	#testOut,r0
	jump	(r0)
	nop

;;; ----------------------------------------
;;; testStore - the DSP's stores RUNS_ST times, compared
;;; r0 - name, r20 - program, r21 - its length (bytes), r22 - 1: Blitter,
;;; r23 - size (4, 2, 1), r24 - text line << 16 (advanced), r9 - results
;;; (advanced)
testStore::
	move	LR,r27
	move	r24,r1
	movei	#PrintString_YX,r4
	BL	(r4)
	moveq	#0,r16			; run
	moveq	#0,r17			; first wrong address
	moveq	#0,r18			; its value
	moveq	#0,r19			; no key long
	moveq	#0,r25			; wrong stores
	moveq	#0,r26			; all ones
	moveq	#0,r8			; the pattern
.run
	movei	#STBUF,r0
	movei	#N_ST,r1
	moveq	#0,r2
.clr	store	r2,(r0)
	subq	#1,r1
	jr	ne,.clr
	addqt	#4,r0
	move	r20,r0
	movei	#DSP_RAM,r1
	move	r21,r2
.cp	load	(r0),r3
	addqt	#4,r0
	subq	#4,r2
	store	r3,(r1)
	jr	ne,.cp
	addqt	#4,r1
	move	r16,r0
	shlq	#3,r0
	sub	r16,r0
	addq	#1,r0
	move	r22,r1
	movei	#ST_WAIT,r2
	movei	#runDsp,r3
	BL	(r3)
	move	r23,r1			; r12/r13: get4/get4p, get2/.., get1/.. by size
	movei	#gets,r12
	shlq	#2,r1
	movei	#getsp,r13
	add	r1,r12
	add	r1,r13
	load	(r12),r12
	load	(r13),r13
	movei	#STBUF,r0
	movei	#N_ST,r2
	movei	#.cmp,r14
	movei	#.same,r15
.cmp	move	r0,r1
	sub	r23,r1			; the one before
	BL	(r12)			; r4 = stored, r5 = expected, r6 = all ones
	cmp	r4,r5
	jump	eq,(r15)
	nop
	addq	#1,r25
	move	r4,r3
	cmpq	#4,r23
	jr	ne,.ones
	nop
	shrq	#16,r3			; a long: its high half
.ones	cmp	r3,r6
	jr	ne,.noff
	nop
	addq	#1,r26
.noff	cmpq	#4,r23
	jr	ne,.prev
	nop
	move	r4,r1			; a long: its high half twice?
	move	r4,r3
	shrq	#16,r1
	shlq	#16,r3
	shrq	#16,r3
	cmp	r1,r3
	jr	ne,.first
	nop
	jr	.first
	addq	#1,r8
.prev	BL	(r13)			; a word or a byte: r5 = the one stored before
	cmp	r4,r5
	jr	ne,.first
	nop
	addq	#1,r8
.first	cmpq	#0,r17
	jr	ne,.same
	nop
	move	r0,r17
	move	r4,r18
.same	subq	#1,r2
	jump	ne,(r14)
	add	r23,r0
	addq	#1,r16
	movei	#RUNS_ST,r0
	movei	#.run,r1
	cmp	r0,r16
	jump	ne,(r1)
	nop
	movei	#testOut,r0
	jump	(r0)
	nop

;;; ----------------------------------------
;;; the line of a test: wrong, all ones, pattern; next line: first wrong
;;; (address, value), key long. r24 - line << 16 (advanced), r27 - return
testOut::
	store	r25,(r9)
	addqt	#4,r9
	store	r26,(r9)
	addqt	#4,r9
	store	r8,(r9)
	addqt	#4,r9
	store	r17,(r9)
	addqt	#4,r9
	store	r18,(r9)
	addqt	#4,r9
	store	r19,(r9)
	addqt	#4,r9
	move	r25,r0
	move	r24,r1
	addq	#24,r1
	movei	#PrintDEC_YX,r4
	BL	(r4)
	move	r26,r0
	move	r24,r1
	addq	#31,r1
	movei	#PrintDEC_YX,r4
	BL	(r4)
	move	r8,r0
	move	r24,r1
	addq	#31,r1
	addq	#7,r1
	movei	#PrintDEC_YX,r4
	BL	(r4)
	movei	#1<<16,r0
	add	r0,r24
	move	r17,r0
	move	r24,r1
	addq	#4,r1
	movei	#PrintHEX_YX,r4
	BL	(r4)
	move	r18,r0
	move	r24,r1
	addq	#14,r1
	movei	#PrintHEX_YX,r4
	BL	(r4)
	move	r19,r0
	move	r24,r1
	addq	#25,r1
	movei	#PrintHEX_YX,r4
	BL	(r4)
	movei	#1<<16,r0
	jump	(r27)
	add	r0,r24

;;; ----------------------------------------
;;; get4/get2/get1 - r0: the store at r0 into r4, the expected one there in
;;; r5, all ones in r6; at r1 only r5 (the second call, from .prev)
;;; chosen by r23 in testStore (r12, r13). r7 destroyed
get4:
	load	(r0),r4
	move	r0,r5
	movei	#KEY,r7
	movei	#$ffff,r6
	jump	(LR)
	xor	r7,r5
get4p:
	move	r1,r5
	movei	#KEY,r7
	jump	(LR)
	xor	r7,r5
get2:
	loadw	(r0),r4
	move	r0,r5
	movei	#WMASK,r7
	shrq	#1,r5
	movei	#$ffff,r6
	and	r7,r5
	movei	#WBIT,r7
	jump	(LR)
	or	r7,r5
get2p:
	move	r1,r5
	movei	#WMASK,r7
	shrq	#1,r5
	and	r7,r5
	movei	#WBIT,r7
	jump	(LR)
	or	r7,r5
get1:
	loadb	(r0),r4
	move	r0,r5
	movei	#BMASK,r7
	movei	#$ff,r6
	and	r7,r5
	movei	#BBIT,r7
	jump	(LR)
	or	r7,r5
get1p:
	move	r1,r5
	movei	#BMASK,r7
	and	r7,r5
	movei	#BBIT,r7
	jump	(LR)
	or	r7,r5

	include <js/inc/txtscr.inc>

	long
gets:		dc.l 0,get1,get2,0,get4		; by size
getsp:		dc.l 0,get1p,get2p,0,get4p
pokes:		dc.l $1000,$12345678, $1010,$9abcdef0, $1800,$cafef00d
		dc.l $3ff8,$0badc0de, $2000,$76543210, $2804,$55aa33cc
;;; program, length, expected, key long, name
tab_ref:	dc.l ref0110,ref0110_end-ref0110,exp0110,$181c,n_0110
		dc.l ref0195,ref0195_end-ref0195,exp0195,$1808,n_0195
		dc.l 0
;;; program, length, Blitter, size, name
tab_st:		dc.l MODstart_dst_l,MODlen_dst_l,1,4,n_l_bl
		dc.l MODstart_dst_lr,MODlen_dst_lr,1,4,n_lr_bl
		dc.l MODstart_dst_l,MODlen_dst_l,0,4,n_l
		dc.l MODstart_dst_w,MODlen_dst_w,1,2,n_w_bl
		dc.l MODstart_dst_w,MODlen_dst_w,0,2,n_w
		dc.l MODstart_dst_b,MODlen_dst_b,1,1,n_b_bl
		dc.l MODstart_dst_b,MODlen_dst_b,0,1,n_b
		dc.l 0

title:		dc.b "DSP stores losing the bus (open bus test)",0
header:		dc.b "test                     wrong   ones   patt",0
n_0110:		dc.b "ref 0110 (256 runs)",0
n_0195:		dc.b "ref 0195 (256 runs)",0
n_l_bl:		dc.b "long, Blitter",0
n_lr_bl:	dc.b "long+load, Blitter",0
n_l:		dc.b "long, quiet",0
n_w_bl:		dc.b "word, Blitter",0
n_w:		dc.b "word, quiet",0
n_b_bl:		dc.b "byte, Blitter",0
n_b:		dc.b "byte, quiet",0
done_txt:	dc.b "done",0

	align	4
ref0110:	ibytes	"ref0110.bin"
ref0110_end:
	align	4
ref0195:	ibytes	"ref0195.bin"
ref0195_end:
	align	4
exp0110:	ibytes	"exp0110.bin"
	align	4
exp0195:	ibytes	"exp0195.bin"
	align	4
ASCII:
	ibytes	<font/light8x8.fnt>
	long
obl:	ibytes	"obl.bin"
obl_end:

	align	32
ende:		equ *
	echo "ENDE : %Hende"
 IF ende > SAVE
	fail "program overlaps SAVE"
 ENDIF
	end
