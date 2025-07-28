all:
	$(MAKE) -C src all
%:
	$(MAKE) -C src $@
