SHELL=bash
# Koch level
L=3
# words per minute
W=15
TARGET=dednnsot
CXXFLAGS=-fmax-errors=6 -std=c++23
CXXLIBRARIES=-lportaudio

.PHONY: default
default: run

$(TARGET): $(TARGET).cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(CXXLIBRARIES)

.PHONY: run
run: $(TARGET)
	./$(TARGET)

.PHONY: train
train: $(TARGET)
	./$(TARGET) -l$L -w$W |& tee logs/l$L_w$W_$(shell date "+%F_%H-%M-%S")
