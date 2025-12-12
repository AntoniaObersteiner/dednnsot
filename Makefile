SHELL=bash
# Koch level
L=3
# words per minute
W=15
CXXFLAGS=-fmax-errors=6 -std=c++23
CXXLIBRARIES=-lportaudio

.PHONY: default
default: run

morse: morse.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(CXXLIBRARIES)

.PHONY: run
run: morse
	./morse

.PHONY: train
train: morse
	./morse -l$L -w$W |& tee logs/l$L_w$W_$(shell date "+%F_%H-%M-%S")
