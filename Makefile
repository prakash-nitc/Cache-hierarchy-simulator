# Cache Hierarchy Simulator — build
# Usage:  mingw32-make            (Windows / MinGW)   or   make   (Linux/macOS)
#         mingw32-make run        build then echo the golden trace
#         mingw32-make clean      remove build artifacts

CXX      := g++
# -MMD -MP: emit a .d dependency file per object so editing a header rebuilds
# every .cpp that includes it (without this, stale objects built against an
# older class layout can link "successfully" and crash at runtime).
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude -MMD -MP

# Append .exe on Windows so the linker output and the Make target name agree
# (otherwise Make never sees the target as "built" and relinks every time).
ifeq ($(OS),Windows_NT)
EXE := .exe
endif

TARGET   := cachesim$(EXE)
SRCDIR   := src
BUILDDIR := build

SRCS := $(wildcard $(SRCDIR)/*.cpp)
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))

TRACEGEN := tracegen$(EXE)

.PHONY: all run clean traces

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Pull in the auto-generated header dependencies (absent on first build).
-include $(OBJS:.o=.d)

run: $(TARGET)
	./$(TARGET) --trace traces/tiny.trace --l1-size 16 --l1-block 4 --addr-bits 8 --verbose

# Workload generator (SPEC section 12 fallback for machines without Valgrind).
$(TRACEGEN): scripts/tracegen.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

# Generate the standard synthetic workload set used by scripts/sweep.py.
traces: $(TRACEGEN)
	./$(TRACEGEN) matmul 64            > traces/matmul.trace
	./$(TRACEGEN) listwalk 4096 262144 > traces/listwalk.trace
	./$(TRACEGEN) seqscan 65536 16     > traces/seqscan.trace
	./$(TRACEGEN) randscan 65536 262144 > traces/randscan.trace
	@wc -l traces/matmul.trace traces/listwalk.trace traces/seqscan.trace traces/randscan.trace

clean:
	rm -rf $(BUILDDIR) cachesim cachesim.exe tracegen tracegen.exe tracegen.d
