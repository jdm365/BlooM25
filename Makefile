all: install clean

install:
	python -m pip install .

clean:
	rm -r build dist *.egg-info .cache bm25_model


CXX = g++
CXXFLAGS = -std=c++17 -O3 -march=native -fopenmp -Wall -Wextra -Wpedantic -Werror
INCLUDES = -I./bm25
SRCS = ./local_testing/main.cpp ./bm25/bloom.cpp ./bm25/engine.cpp ./bm25/serialize.cpp ./bm25/vbyte_encoding.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = ./bin/bm25_model

$(TARGET): $(OBJS)
	mkdir -p $(dir $(TARGET))
	$(CXX) $(CXXFLAGS) $(OBJS) $(INCLUDES) -o $@

./%.o: ./%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

# Run the program
run: $(TARGET)
	$(TARGET)
