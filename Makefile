default: tests

forth: forth.cpp
	g++ -g -Wall -Werror -std=gnu++14 -o forth forth.cpp

clean:
	rm -f *.o forth test_cases/*.actual

paste:
	sed -rf pastescript.sed forth.cpp

%.actual: %.fo %.expected forth
	./forth $< > $@ && \
	diff -U5 $*.expected $@

tests: $(patsubst %.fo,%.actual,$(wildcard test_cases/*.fo))

.DELETE_ON_ERROR:
