all:
	g++ main.cpp -std=c++20 -lboost_system -lfmt

run: all
	./a.out
