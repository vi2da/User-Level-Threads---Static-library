#vi2da

CC= g++
CFLAGS= -c -g -Wvla -Wextra -Wall -std=c++11
TAR_TARGET =  Makefile README uthreads.cpp

#all targets
all: uthreads

#library
uthreads: uthreads.o
	ar -rcs libuthreads.a uthreads.o

#object files

uthreads.o: uthreads.cpp uthreads.h
	$(CC) $(CFLAGS) uthreads.cpp

#clean
clean:
	rm -f *.o *.a

#tar
tar: ex2.tar

ex2.tar: $(TAR_TARGET)
	tar -cvf $@ $^

#phony
.PHONY: all clean tar
