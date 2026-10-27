APP      := mothplayer
SRC      := main.cpp audio.cpp ui.cpp
OBJ      := $(SRC:.cpp=.o)
DEP      := $(OBJ:.o=.d)

CXX      ?= g++
PKGCONF  ?= pkg-config

# Dependencies (ncursesw for UTF-8 support)
PKGS := ncursesw libsixel alsa libavformat libavcodec libswresample libswscale libavutil

PKG_CFLAGS := $(shell $(PKGCONF) --cflags $(PKGS))
PKG_LIBS   := $(shell $(PKGCONF) --libs $(PKGS))

OPTFLAGS := -O3 -march=native -flto -fno-plt -fomit-frame-pointer -pipe
WARNFLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

CXXFLAGS += -std=c++20 $(OPTFLAGS) $(WARNFLAGS) -DNDEBUG -MMD -MP -pthread $(PKG_CFLAGS)
LDFLAGS  += -flto -pthread -Wl,-O2 -Wl,--as-needed -Wl,--gc-sections

.PHONY: all clean run

all: $(APP)

$(APP): $(OBJ)
	@echo "  LD      $@"
	@$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(PKG_LIBS)

%.o: %.cpp
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@rm -f $(OBJ) $(APP) $(DEP)

run: $(APP)
	@./$(APP)

-include $(DEP)
