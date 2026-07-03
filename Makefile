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

.PHONY: all run clean

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

clean:
	rm -rf $(BUILDDIR) cachesim cachesim.exe
