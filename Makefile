CXX ?= c++
AR ?= ar
INSTALL ?= install
PREFIX ?= /usr/local

CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
CPPFLAGS ?= -I.
ARFLAGS ?= rcs

LIB_NAME := liblinuxfs_mmap.a
TEST_NAME := mmap_test

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
LIB_SOURCE := mmap.cpp
BUILD_TEST := 1
else
LIB_SOURCE := mmap_unsupported.cpp
BUILD_TEST := 0
endif

LIB_OBJECT := $(LIB_SOURCE:.cpp=.o)
TEST_OBJECT := mmap_test.o

.PHONY: all library test check clean install help

all: library $(if $(filter 1,$(BUILD_TEST)),test)

library: $(LIB_NAME)

$(LIB_NAME): $(LIB_OBJECT)
	$(AR) $(ARFLAGS) $@ $^

%.o: %.cpp mmap.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

ifeq ($(BUILD_TEST),1)
test: $(TEST_NAME)

$(TEST_NAME): $(TEST_OBJECT) $(LIB_NAME)
	$(CXX) $(CXXFLAGS) $^ -o $@

check: $(TEST_NAME)
	./$(TEST_NAME)
else
test check:
	@echo "Tests are only available for the Linux mmap backend"
endif

install: library
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/linuxfs
	$(INSTALL) -m 644 $(LIB_NAME) $(DESTDIR)$(PREFIX)/lib/$(LIB_NAME)
	$(INSTALL) -m 644 mmap.hpp $(DESTDIR)$(PREFIX)/include/linuxfs/mmap.hpp

clean:
	rm -f $(LIB_OBJECT) $(TEST_OBJECT) $(LIB_NAME) $(TEST_NAME)

help:
	@echo "make          Build the library and Linux test executable"
	@echo "make library  Build liblinuxfs_mmap.a"
	@echo "make test     Build the test executable"
	@echo "make check    Run the test executable"
	@echo "make install  Install the library and public header"
	@echo "make clean    Remove generated objects, library, and test executable"
