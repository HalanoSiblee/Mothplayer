CXX      := g++
TARGET   := mothplayer
SRC      := src/main.cpp
VERSION := $(shell cat V)
CXXSTD   ?= c++20

CXXFLAGS := -std=$(CXXSTD) -O3 -march=native -mtune=native \
            -flto=auto -fno-plt -fomit-frame-pointer \
            -funroll-loops -pipe -Wall -Wextra
CXXFLAGS += -DAPP_V=\"$(VERSION)\"

LDFLAGS  := -flto=auto -s
LDLIBS   := -lncurses -lasound -lavformat -lavcodec -lswresample -lswscale -lavutil -lsixel -lpthread

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)
