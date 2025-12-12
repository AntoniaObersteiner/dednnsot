
CXXFLAGS=-fmax-errors=6 -std=c++23
CXXLIBRARIES=-lportaudio
morse: morse.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(CXXLIBRARIES)

