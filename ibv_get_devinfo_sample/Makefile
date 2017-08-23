PROGS = ibv_get_devinfo_sample
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

ibv_get_devinfo_sample: ibv_get_devinfo_sample.o
