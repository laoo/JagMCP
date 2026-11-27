;-*-asm-*-
; RUN: --mem $4000:$100 --mem $10000:$100
; The DSP runs a routine from main RAM: each fetch a long word through its gateway, on Jerry's
; 16-bit bus. It first waits in its local RAM, off the bus, until the boot program's 68000 has
; stopped (in the simulation a fetch at once takes the bus as the 68000 reads its STOP's operand,
; which then gets the DSP's word), then copies the routine to $4000 and jumps there. The routine's
; loop interleaves the fetches with word stores to main RAM and to Tom's CLUT, word and long loads
; of both, loads of its local RAM (a load and the fetch after it queued in the gateway), a JUMP and
; its delay slot; then it jumps back to the local RAM and stops.
	dsp
	include "jaghard.inc"
MAIN	equ	$4000
	RUN	D_RAM
	movei	#200,r0
.wait	subq	#1,r0
	jr	ne,.wait
	nop
	movei	#main,r0
	movei	#MAIN,r6
	movei	#(main_e-main)/4,r7
.copy	load	(r0),r8
	addq	#4,r0
	store	r8,(r6)
	subq	#1,r7
	jr	ne,.copy
	addq	#4,r6
	movei	#MAIN,r0
	jump	(r0)
	nop
stop	movei	#D_CTRL,r30
	moveq	#0,r31
.w	store	r31,(r30)
	jr	.w
	nop
	nop

	long
main	movei	#$10000,r14
	movei	#$f00400,r5
	moveq	#0,r1
	movei	#20,r2
	movei	#MAIN+.loop-main,r9
	movei	#D_RAM,r10
.loop	storew	r1,(r14)
	addq	#2,r14
	move	r5,r8
	storew	r1,(r5)
	addq	#2,r8
	move	r1,r4
	neg	r4
	storew	r4,(r8)
	loadw	(r5),r6
	load	(r5),r7
	add	r6,r1
	add	r7,r1
	addq	#4,r5
	load	(r14),r4
	load	(r10),r11
	add	r4,r1
	add	r11,r1
	addq	#4,r10
	addq	#3,r1
	subq	#1,r2
	jump	ne,(r9)
	or	r1,r4
	movei	#$10080,r14
	storew	r1,(r14)
	addq	#2,r14
	shrq	#16,r1
	storew	r1,(r14)
	movei	#stop,r0
	jump	(r0)
	nop
	long
main_e
	END
