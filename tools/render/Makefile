# Renders the firmware's screen on the host, as SVG.
#
#   make            every state into out/
#   make one STATE=route
#   make clean
#
# It compiles firmware/src/screen.cpp — the file the board draws with — against
# shim/, so the picture is the firmware's own layout and not a second drawing of
# it. Nothing here goes into the firmware build.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter
FW       := ../../firmware
INCLUDES := -Ishim -I$(FW)/include

BIN := render

.PHONY: all one clean

all: $(BIN)
	@mkdir -p out
	@./$(BIN)

one: $(BIN)
	@mkdir -p out
	@./$(BIN) --only $(STATE)

$(BIN): render.cpp $(FW)/src/screen.cpp shim/M5Unified.h $(FW)/include/magpie.h $(FW)/include/screen.h $(FW)/include/bird.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ render.cpp $(FW)/src/screen.cpp

clean:
	@rm -rf $(BIN) out
