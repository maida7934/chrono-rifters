CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -pthread
# ncurses for TUI (Section 9). Uncomment SFML/SDL line if switching to GUI.
LIBS     := -lncurses -lrt -lpthread

SHARED_SRC := shared/weapon_table.cpp

ARB_SRC  := arbiter/arbiter.cpp $(SHARED_SRC)
HIP_SRC  := hip/hip.cpp         $(SHARED_SRC)
ASP_SRC  := asp/asp.cpp         $(SHARED_SRC)

# Include shared/ so all .h files resolve without path prefixes
INC      := -I.

.PHONY: all clean

all: arbiter_bin hip_bin asp_bin

arbiter_bin: $(ARB_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $^ -o $@ $(LIBS)

hip_bin: $(HIP_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $^ -o $@ $(LIBS)

asp_bin: $(ASP_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $^ -o $@ $(LIBS)

test_deadlock: test_deadlock.cpp $(SHARED_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $^ -o $@ -lrt -lpthread

clean:
	rm -f arbiter_bin hip_bin asp_bin test_deadlock