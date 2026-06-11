# Native (host) build of the IIgs disk-format regression tests.
# This deliberately uses the host compiler, NOT the ARM cross-toolchain, so the
# codec can be exercised on a dev machine without a MiSTer.
#
#   make        # build
#   make run    # build + run the tests
#   make clean

CXX      ?= c++
CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++11
# Where the real disk-image fixtures live (override: make run FIXTURES=/path)
FIXTURES ?= ../../../artifacts_for_iigs

BIN = test_iigs_fmt
SRC = test_iigs_fmt.cpp ../iigs_fmt.cpp

$(BIN): $(SRC) ../iigs_fmt.h
	$(CXX) $(CXXFLAGS) -I.. -o $@ $(SRC)

run: $(BIN)
	./$(BIN) "$(FIXTURES)"

clean:
	rm -f $(BIN)

.PHONY: run clean
