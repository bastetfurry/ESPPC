# PC build of the emulator, for testing. Not used by PlatformIO.
#   make            builds ./pctool
#   make clean
#
# The emulator's own sources are compiled as they are; stub/ stands in for FabGL's hardware side,
# ESP-IDF and FreeRTOS, and host.cpp implements those stand-ins (see the comments there).

CXX      ?= g++
CXXFLAGS ?= -O2 -g -std=gnu++17 -w
CPPFLAGS := -DFABGL_EMULATED -Istub -I../lib/FabGL/src -I../src

SRC = main.cpp host.cpp \
      ../src/machine.cpp ../src/bios.cpp ../src/ega.cpp ../src/fdc.cpp ../src/hdc.cpp \
      ../src/mm58174.cpp ../src/sn76496.cpp ../src/tandydac.cpp ../src/adlib.cpp \
      $(wildcard ../src/opl/*.cpp) \
      ../lib/FabGL/src/emudevs/i8086.cpp ../lib/FabGL/src/emudevs/PIC8259.cpp \
      ../lib/FabGL/src/emudevs/PIT8253.cpp ../lib/FabGL/src/emudevs/MC146818.cpp \
      ../lib/FabGL/src/emudevs/i8042.cpp ../lib/FabGL/src/emudevs/PC8250.cpp \
      ../lib/FabGL/src/emudevs/graphicsadapter.cpp

OBJ = $(patsubst %.cpp,obj/%.o,$(notdir $(SRC)))
DEP = $(OBJ:.o=.d)

vpath %.cpp . ../src ../src/opl ../lib/FabGL/src/emudevs

pctool: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

obj/%.o: %.cpp | obj
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

obj:
	mkdir -p obj

clean:
	rm -rf obj pctool

-include $(DEP)

.PHONY: clean
