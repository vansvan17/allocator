CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic

.PHONY: test sanitize tsan clean

test: build/test_allocator
	./build/test_allocator

build/test_allocator: test_allocator.cpp arena_allocator.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) test_allocator.cpp -o $@

sanitize:
	$(MAKE) clean
	$(MAKE) CXXFLAGS='$(CXXFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' test

tsan:
	$(MAKE) clean
	$(MAKE) CXXFLAGS='$(CXXFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=thread' test

clean:
	rm -rf build
