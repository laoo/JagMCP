| RUN: --m68k --ntsc --cycles 3000 --mem $10000:$4
| An NTSC console (--ntsc): JOYBUTS read with the pads released and stored in main RAM, its bit 4
| set (jaguar.v b[4], the core's ntsc input), which the BIOS reads to choose the video timing.
	.text
	move.w	#0x0100,0xf14000	| JOYSTICK: the outputs disabled
	move.w	0xf14000,0x10000
	move.w	0xf14002,0x10002	| JOYBUTS
	stop	#0x2700
