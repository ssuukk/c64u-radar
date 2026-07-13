# C64U Radar — public build
# Requires cc65/cl65 on PATH.

PRG = c64u_radar.prg
MAP = c64u_radar.map
SRC = c64u_radar.c

all: $(PRG)

$(PRG): $(SRC)
	cl65 -t c64 -O -I. --mapfile $(MAP) -o $@ $(SRC)
	@python3 check_map.py $(MAP)

clean:
	rm -f $(PRG) $(MAP) *.o

host_test:
	cc -DHOST_TEST -I. -Ihost_test -o host_test/harness host_test/harness.c
