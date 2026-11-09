;;; Object list of store_test: the text bitmap only
	.include "68k_inc/jaguar.inc"
y_txt	= 44			; above about 30 lines the console cuts off
	.objproc
	.org	$800
 branch VC < y_txt, _stop
 branch VC > y_txt+8*28, _stop
 bitmap	$140000,0,y_txt,384/64,384/64,8*28,0,127,NOTRANS RELEASE,0,1
_stop:
 stop
	.qphrase
