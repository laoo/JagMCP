;-*-asm-*-
; RUN: --gpu --mem $30000:$40
; The blitter reads the DSP's space through Jerry's slave: the GPU writes eight 16 bit pixels into
; the DSP's RAM ($F1B000); the blitter copies them to $30000 in pixel mode, to $30010 in phrase
; mode, and to $30020 in phrase mode with 32 bit pixels.
	gpu
	include "jaghard.inc"

; copies a line of \3 pixels from \0 to \1, flags \2
	macro BLIT
	movei	#\1,r0
	store	r0,(r14)			; A1_BASE: the destination
	movei	#\2,r0
	store	r0,(r14+4)			; A1_FLAGS
	store	r0,(r14+$28)			; A2_FLAGS
	movei	#\0,r0
	store	r0,(r14+$24)			; A2_BASE: the source
	moveq	#0,r0
	store	r0,(r14+$c)			; A1_PIXEL
	store	r0,(r14+$30)			; A2_PIXEL
	movei	#$00010000|\3,r0
	store	r0,(r14+$3c)			; B_COUNT: a line of \3
	movei	#SRCEN|LFU_S,r0
	store	r0,(r14+$38)			; B_CMD
.\w	load	(r14+$38),r1
	btst	#0,r1
	jr	eq,.\w
	nop
	endm

	RUN $F03000
	movei	#$F1B000,r1	; four long words of a sequence
	movei	#$12345678,r3
	movei	#$01030507,r4
	moveq	#4,r5
.fill	store	r3,(r1)
	addq	#4,r1
	subq	#1,r5
	jr	ne,.fill
	add	r4,r3
	movei	#A1_BASE,r14
	BLIT	$F1B000,$30000,PITCH1|PIXEL16|WID64|XADDPIX,8	; pixel mode
	BLIT	$F1B000,$30010,PITCH1|PIXEL16|WID64|XADDPHR,8	; phrase mode
	BLIT	$F1B000,$30020,PITCH1|PIXEL32|WID64|XADDPHR,4	; phrase mode, 32 bit pixels
.done	addq	#1,r6
	jr	.done
	nop
	END
