CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread
LIBS = -lncurses -lrt

# Use the actual file names as the targets
all: arbiter_exec hip_exec asp_exec
	@echo "Build complete."

arbiter_exec: arbiter/arbiter.cpp
	$(CXX) $(CXXFLAGS) arbiter/*.cpp shared/*.cpp -o arbiter_exec $(LIBS)

hip_exec: hip/hip.cpp
	$(CXX) $(CXXFLAGS) hip/*.cpp shared/*.cpp -o hip_exec $(LIBS)

asp_exec: asp/asp.cpp
	$(CXX) $(CXXFLAGS) asp/*.cpp shared/*.cpp -o asp_exec $(LIBS)

clean:
	rm -f arbiter_exec hip_exec asp_exec

.PHONY: all clean