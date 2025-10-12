SRCS := $(wildcard *.cpp)
EXES := $(SRCS:%.cpp=%)
IR_FILES := $(SRCS:%.cpp=%.ll)
CLANGXX := /work/llvm-project-focs/build/bin/clang++ # in container

CLANGFLAGS := -std=c++11 -fsanitize=thread
# CLANGFLAGS := -std=c++11 -fno-sanitize=thread

.PHONY: all ir clean rebuild

all: $(EXES)

%: %.cpp
	$(CLANGXX) $(CLANGFLAGS) $^ -o $@

ir: $(IR_FILES)

%.ll: %.cpp
	$(CLANGXX) $(CLANGFLAGS) -emit-llvm -S -o $@ $^

clean:
	$(RM) $(EXES) $(IR_FILES)

rebuild: clean all

