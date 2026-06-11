# Native (host) build of the iigs_convert CLI tool.
CXX      ?= c++
CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++11

BIN = iigs_convert
SRC = iigs_convert.cpp ../iigs_fmt.cpp

$(BIN): $(SRC) ../iigs_fmt.h
	$(CXX) $(CXXFLAGS) -I.. -o $@ $(SRC)

clean:
	rm -f $(BIN)

.PHONY: clean
