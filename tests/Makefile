# Makefile for FleX-Chai3d unit tests
# Run from the repository root:  make -C tests
# Or from the tests/ directory:  make

CXX      ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2

.PHONY: all run clean

all: test_dicomvolume_coords

test_dicomvolume_coords: test_dicomvolume_coords.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

run: test_dicomvolume_coords
	./test_dicomvolume_coords

clean:
	rm -f test_dicomvolume_coords
