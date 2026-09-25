# ====== Moth Player Makefile ======
APP     := mothplayerx11
SRC     := main.cpp
OBJ     := $(SRC:.cpp=.o)

CXX     ?= g++
STRIP   ?= strip

# Optimization flags - aggressive
OPTFLAGS := -O3 -march=native -mtune=native -flto=auto -fno-plt \
            -fomit-frame-pointer -fno-semantic-interposition \
            -funroll-loops -fstrict-aliasing -fno-math-errno \
            -fno-trapping-math -fno-signed-zeros -ffast-math

WARNFLAGS := -Wall -Wextra -Wno-unused-parameter

CXXFLAGS ?= -std=c++17 $(OPTFLAGS) $(WARNFLAGS) -DNDEBUG

# LTO linker flags
LDFLAGS += -flto=auto -Wl,-O1 -Wl,--as-needed -Wl,--gc-sections \
           -Wl,-z,relro,-z,now -Wl,--strip-all

# pkg-config packages
PKGS := x11 xrender xft freetype2 fontconfig alsa \
        libavformat libavcodec libswresample libswscale libavutil

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

CXXFLAGS += $(PKG_CFLAGS)
LDLIBS   := $(PKG_LIBS) -lpthread

.PHONY: all clean run debug

all: $(APP)

$(APP): $(OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	$(STRIP) --strip-all $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(APP)

run: $(APP)
	./$(APP)

debug: CXXFLAGS := -std=c++17 -O0 -g3 -DDEBUG $(shell pkg-config --cflags $(PKGS))
debug: LDFLAGS :=
debug: clean $(APP)

-include $(OBJ:.o=.d)