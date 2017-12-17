CXX=clang++
CXXFLAGS=-std=c++11 -Wall -Wextra -pedantic -fsanitize=address -fsanitize=undefined -g

ht: ht.cpp ht.h
	$(CXX) $(CXXFLAGS) -o ht ht.cpp

clean:
	rm -f ht
