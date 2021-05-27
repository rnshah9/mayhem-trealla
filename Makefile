GIT_VERSION := "$(shell git describe --abbrev=4 --dirty --always --tags)"

CFLAGS = -Isrc -I/usr/local/include -DVERSION='$(GIT_VERSION)' -O3 \
	$(OPT) -Wall -Wextra -D_GNU_SOURCE -Wno-deprecated-declarations

LDFLAGS = -lreadline -L/usr/local/lib -lm

ifndef NOSSL
CFLAGS += -DUSE_OPENSSL=1
LDFLAGS += -lssl -lcrypto
endif

ifdef INT128
CFLAGS += -DUSE_INT128=1
else ifdef INT32
CFLAGS += -DUSE_INT32=1
endif

ifdef GMP
CFLAGS += -DUSE_GMP=1
LDFLAGS += -lgmp
endif

ifdef THREADS
CFLAGS += -DUSE_THREADS=1 -pthread
LDFLAGS += -pthread
endif

ifdef LTO
CFLAGS += -flto=$(LTO)
LDFLAGS += -flto=$(LTO)
endif

OBJECTS = tpl.o src/history.o src/functions.o \
	src/predicates.o src/contrib.o src/heap.c \
	src/library.o src/parser.o src/print.o src/query.o \
	src/skiplist.o src/base64.o src/network.o src/utf8.o

OBJECTS +=  library/builtins.o library/lists.o library/apply.o \
	library/http.o library/atts.o library/error.o library/dcgs.o \
	library/format.o library/charsio.o library/freeze.o \
	library/ordsets.o library/assoc.o library/dict.o library/dif.o

library/%.c: library/%.pl
	xxd -i $^ $@

all: tpl

tpl: $(OBJECTS)
	$(CC) -o tpl $(OBJECTS) $(OPT) $(LDFLAGS)

profile:
	$(MAKE) 'OPT=$(OPT) -O0 -pg -DDEBUG'

debug:
	$(MAKE) 'OPT=$(OPT) -O0 -g -DDEBUG'

test:
	./tests/run.sh

clean:
	rm -f tpl src/*.o library/*.o library/*.c *.o gmon.* vgcore.* *.core core core.* faultinject.*

# from [gcc|clang] -MM *.c

base64.o: src/base64.c src/base64.h
contrib.o: src/contrib.c src/trealla.h src/internal.h src/map.h \
  src/skiplist.h src/cdebug.h src/builtins.h
functions.o: src/functions.c src/trealla.h src/internal.h src/map.h \
  src/skiplist.h src/cdebug.h src/query.h src/builtins.h
heap.o: src/heap.c src/trealla.h src/internal.h src/map.h src/skiplist.h \
  src/cdebug.h src/query.h src/builtins.h src/heap.h
history.o: src/history.c src/history.h src/utf8.h src/cdebug.h
library.o: src/library.c src/library.h
network.o: src/network.c src/internal.h src/map.h src/skiplist.h \
  src/trealla.h src/cdebug.h src/network.h
parser.o: src/parser.c src/internal.h src/map.h src/skiplist.h \
  src/trealla.h src/cdebug.h src/history.h src/library.h src/parser.h \
  src/module.h src/prolog.h src/query.h src/builtins.h src/heap.h \
  src/utf8.h
predicates.o: src/predicates.c src/trealla.h src/internal.h src/map.h \
  src/skiplist.h src/cdebug.h src/network.h src/base64.h src/library.h \
  src/parser.h src/module.h src/prolog.h src/query.h src/builtins.h \
  src/heap.h src/utf8.h
print.o: src/print.c src/internal.h src/map.h src/skiplist.h \
  src/trealla.h src/cdebug.h src/parser.h src/module.h src/query.h \
  src/builtins.h src/network.h src/utf8.h
query.o: src/query.c src/internal.h src/map.h src/skiplist.h \
  src/trealla.h src/cdebug.h src/history.h src/parser.h src/module.h \
  src/prolog.h src/query.h src/builtins.h src/heap.h src/utf8.h
skiplist.o: src/skiplist.c src/skiplist.h
utf8.o: src/utf8.c src/utf8.h
