CXX=clang++
CXXFLAGS=-std=c++11 -Wall -Wextra -pedantic -I./benchmark/include -L./benchmark/src
DEBUG_FLAGS=-g -fsanitize=address -fsanitize=undefined
RELEASE_FLAGS=-DNDEBUG -O2
TARGETS = ht bench

all: $(TARGETS)

ht: ht.cpp ht.h
	$(CXX) $(CXXFLAGS) $(DEBUG_FLAGS) -o ht ht.cpp

bench: bench.cpp ht.h
	$(CXX) $(CXXFLAGS) $(RELEASE_FLAGS) -o bench bench.cpp -lbenchmark

clean:
	rm -f $(TARGETS)
