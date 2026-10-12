KDIR ?= /lib/modules/$(shell uname -r)/build
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

all: module tool

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel modules

tool: userspace/myfs_tool

userspace/myfs_tool: userspace/myfs_tool.cpp include/myfs_ioctl.h
	$(CXX) $(CXXFLAGS) -Iinclude -o $@ $<

check: all
	sh tests/selftest.sh

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel clean
	rm -f userspace/myfs_tool

.PHONY: all module tool check clean
