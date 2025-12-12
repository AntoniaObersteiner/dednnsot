
CXXFLAGS=-fmax-errors=6 -std=c++23
CXXLIBRARIES=-lportaudio

.PHONY: default
default: run

morse: morse.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(CXXLIBRARIES)

.PHONY: run
run: morse
	./morse
