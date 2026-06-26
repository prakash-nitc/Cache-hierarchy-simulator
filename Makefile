# Cache Hierarchy Simulator — build
# Usage:  mingw32-make            (Windows / MinGW)   or   make   (Linux/macOS)
#         mingw32-make run        build then echo the golden trace
#         mingw32-make clean      remove build artifacts

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude

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

run: $(TARGET)
	./$(TARGET) --trace traces/tiny.trace

clean:
	rm -rf $(BUILDDIR) cachesim cachesim.exe
