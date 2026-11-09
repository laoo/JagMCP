# DSP stores and the open bus: console test (JagMCP PR #7). Needs BJL_ROOT
# (new_bjl), lyxass, rmac. exp*.bin: $1000-$3FFF after JagMCP's run of the
# reference without the open bus (make expected, needs the JagMCP server);
# exp0195.bin then from this program's run in JagMCP (openbus_test.js)

DEMO=openbus_test
RMAC=rmac -4 ~oall -i$(BJL_ROOT)
TJASS=lyxass -sh -nr -w
REFS=$(HOME)/jaguar/JagMCP/tests/reference

all: $(DEMO).j64

obl.bin: obl.S
	$(RMAC) -fr $< -o $@

ref%.bin: ref%.o
	tail -c +13 $< > $@		# without the BS94 header (magic, address, length)

$(DEMO).o: $(DEMO).js obl.bin ref0110.bin ref0195.bin exp0110.bin exp0195.bin
	$(TJASS) $<

$(DEMO).j64: $(DEMO).o
	cat sbl.XXX $< > $@
	@n=$$(wc -c < $@); size=16384; while [ $$size -lt $$n ]; do size=$$((size*2)); done; \
	bzcat $(BJL_ROOT)/bin/allff.bin.bz2 >> $@; truncate -s $$size $@; \
	echo "$@: $$n bytes, padded to $$size"

.PHONY: expected
expected:
	for n in 0110 0195; do \
	  jagmcp.py emu.load $(REFS)/random_external_$$n.o --no-open-bus \
	    $$(sed 's/--cycles [0-9]*//; s/--mem [^ ]*//g' $(REFS)/random_external_$$n.args) >/dev/null; \
	  jagmcp.py emu.run_cycles count=20000 >/dev/null; \
	  jagmcp.py --json memory.read address=0x1000 size=12288 | \
	    python3 -c "import json,sys; sys.stdout.buffer.write(bytes.fromhex(json.load(sys.stdin)['data_hex']))" > exp$$n.bin; \
	done

clean:
	rm -f *.o *.equ obl.bin $(DEMO).j64
