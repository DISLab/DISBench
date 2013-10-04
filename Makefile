CC=g++ 
SSEFLAGS= -O2 -msse -msse2 -msse3 -static
#SSEFLAGS= -O2 -ftree-vectorize -msse2 -ftree-vectorizer-verbose=2 #-static -msse -msse2 -msse3 
CFLAGS=-I. -Wall  $(SSEFLAGS) 
LDFLAGS=-lstdc++ 
PAPI=-DPAPI_USED
all: disbench

disbench: disbench.o
	$(CC) -o $@ $< $(LDFLAGS) $(LIBS) -lpthread -lpapi #-lpfm
	
%.o:%.cpp
	$(CC) $(PAPI) -c -o $@ $< $(CFLAGS) 
%.S:%.cpp
	$(CC) -S -o $@ $< $(CFLAGS) 
clean:
	rm -f disbench
	rm -f *.o
