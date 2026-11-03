CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
BINARY := raycasting

.PHONY: all run test screenshots clean

all: $(BINARY)

$(BINARY): raycasting.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

run: $(BINARY)
	./$(BINARY)

test: $(BINARY)
	./$(BINARY) --selftest

screenshots: $(BINARY)
	mkdir -p docs
	./$(BINARY) --render docs/raycasting-3d.ppm --width 960 --height 540 --minimap
	./$(BINARY) --render docs/raycasting-rays.ppm --width 960 --height 540 --map
	python3 tools/ppm_to_png.py docs/raycasting-3d.ppm docs/raycasting-3d.png
	python3 tools/ppm_to_png.py docs/raycasting-rays.ppm docs/raycasting-rays.png
	rm -f docs/raycasting-3d.ppm docs/raycasting-rays.ppm

clean:
	rm -f $(BINARY)
