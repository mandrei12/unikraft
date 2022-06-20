UK_ROOT ?= $(PWD)/../../../unikraft
UK_LIBS ?= $(PWD)/../../lib
LIBS := $(UK_LIBS)/lib-newlib
#LIBS :=

all:
	@$(MAKE) -C $(UK_ROOT) A=$(PWD) L=$(LIBS)

$(MAKECMDGOALS):
	@$(MAKE) -C $(UK_ROOT) A=$(PWD) L=$(LIBS) $(MAKECMDGOALS)
