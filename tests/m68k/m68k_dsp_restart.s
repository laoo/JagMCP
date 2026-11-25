| RUN: --m68k --cycles 20000 --mem $10000:$4
| The DSP, loading main RAM a long word a pass, is stopped and started again on one that loads
| main RAM and counts its passes in its RAM; the 68000 writes $600DD00E at $10000 if it counted.
	.text
	lea	proga,%a0
	lea	0xf1b000,%a1
	moveq	#7,%d0
1:	move.l	(%a0)+,(%a1)+
	dbra	%d0,1b
	move.l	#0x20000,0xf1cf80
	move.l	#0xf1b000,0xf1a110	| D_PC
	move.l	#1,0xf1a114		| D_CTRL: GO
	move.l	#200,%d0
2:	subq.l	#1,%d0
	bne.s	2b
	clr.l	0xf1a114		| stopped, a load most likely under way
	lea	progb,%a0
	lea	0xf1b100,%a1
	moveq	#8,%d0
3:	move.l	(%a0)+,(%a1)+
	dbra	%d0,3b
	clr.l	0xf1cf84
	move.l	#0xf1b100,0xf1a110
	move.l	#1,0xf1a114
	move.l	#40,%d0
4:	subq.l	#1,%d0
	bne.s	4b
	move.l	#0xbad0bad0,%d1
	tst.l	0xf1cf84
	beq.s	5f
	move.l	#0x600dd00e,%d1
5:	move.l	%d1,0x10000
	stop	#0x2700

	.even
| A at $F1B000 (the console shares the bus with it): movei #$F1CF80,r14; load (r14),r0;
| movei #$F1CF84,r15; moveq #0,r6; movei #.a,r1; .a: load (r0),r2; addq #1,r6; store r6,(r15);
| jump (r1); nop
| B at $F1B100: the same, its load waited for: movei #$F1CF80,r14; load (r14),r0;
| movei #$F1CF84,r15; moveq #0,r6; movei #.b,r1; .b: load (r0),r2; or r2,r2; addq #1,r6;
| store r6,(r15); jump (r1); nop (rmac)
proga:
	.word	0x980e, 0xcf80, 0x00f1, 0xa5c0, 0x980f, 0xcf84, 0x00f1, 0x8c06, 0x9801, 0xb016
	.word	0x00f1, 0xa402, 0x0826, 0xbde6, 0xd020, 0xe400
progb:
	.word	0x980e, 0xcf80, 0x00f1, 0xa5c0, 0x980f, 0xcf84, 0x00f1, 0x8c06, 0x9801, 0xb116
	.word	0x00f1, 0xa402, 0x2842, 0x0826, 0xbde6, 0xd020, 0xe400, 0xe400
