BIN := $(notdir $(lastword $(abspath .)))

EXT_H    := h
EXT_HPP  := h hh hpp hxx h++
EXT_C    := c
EXT_CXX  := C cc cpp cxx c++

INCLUDE_DIR := ../include
SOURCE_DIR  := .

WILD_EXT  = $(strip $(foreach EXT,$($(1)),$(wildcard $(2)/*.$(EXT))))

HDRS_C   := $(call WILD_EXT,EXT_H,$(INCLUDE_DIR))
HDRS_CXX := $(call WILD_EXT,EXT_HPP,$(INCLUDE_DIR))
SRCS_C   := $(call WILD_EXT,EXT_C,$(SOURCE_DIR))
SRCS_CXX := $(call WILD_EXT,EXT_CXX,$(SOURCE_DIR))
OBJS     := $(SRCS_C:%=%.o) $(SRCS_CXX:%=%.o)

CC       := $(CC)
# CCFLAGS  := -Wall -Wextra -Wfatal-errors -O2 -std=c11 -fPIC -I$(INCLUDE_DIR)
CCFLAGS  := -Wall -Wextra -Wfatal-errors -std=c11 -fPIC -I$(INCLUDE_DIR)
CXX      := $(CXX)
# CXXFLAGS := -Wall -Wextra -Wfatal-errors -O2 -std=c++17 -fPIC -I$(INCLUDE_DIR)
CXXFLAGS := -Wall -Wextra -Wfatal-errors -std=c++17 -fPIC -I$(INCLUDE_DIR)
LD       := $(if $(SRCS_CXX),$(CXX),$(CC))
LDFLAGS  := -shared
LDLIBS   :=

.PHONY: build clean

debug: CXXFLAGS += -DDEBUG -g
debug: CCFLAGS += -DDEBUG -g
debug: build

build: $(BIN)

clean:
	$(RM) $(OBJS) $(BIN)

%.cpp.o: %.cpp $(HDRS_CXX)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BIN): $(OBJS)
	$(CXX) -o $@ $(OBJS) ../src.so $(LDLIBS)
