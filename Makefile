CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
BINARY := game3d
SOURCES := $(wildcard src/*.cpp)
HEADERS := $(wildcard src/*.hpp)

.PHONY: all run test screenshots clean

all: $(BINARY)

$(BINARY): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES)

run: $(BINARY)
	./$(BINARY)

test: $(BINARY)
	./$(BINARY) --selftest

screenshots: $(BINARY)
	mkdir -p docs
	./$(BINARY) --render docs/3d-first-person.ppm --width 960 --height 540 --minimap
	./$(BINARY) --render docs/3d-overhead.ppm --width 960 --height 540 --map
	./$(BINARY) --render docs/3d-driving.ppm --width 960 --height 540 --drive 0
	python3 tools/ppm_to_png.py docs/3d-first-person.ppm docs/3d-first-person.png
	python3 tools/ppm_to_png.py docs/3d-overhead.ppm docs/3d-overhead.png
	python3 tools/ppm_to_png.py docs/3d-driving.ppm docs/3d-driving.png
	rm -f docs/3d-first-person.ppm docs/3d-overhead.ppm docs/3d-driving.ppm

clean:
	rm -f $(BINARY)
