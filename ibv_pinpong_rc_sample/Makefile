PROGS = ibv_pingpong_rc_sample
CFLAGS += -g
LIBS = -libverbs

LINKER=$(CC)
ifeq ($(COMPILE_LANG), cpp)
LINKER=$(GCXX)
endif

all: ${PROGS}
clean:
	rm -f ${PROGS} *.o

.c.o:
	$(CC) -c -o $@ $(CFLAGS) $<

$(PROGS):
	$(LINKER) -o $@ $^ $(LIBS)

ibv_pingpong_rc_sample: ibv_pingpong_rc_sample.o
