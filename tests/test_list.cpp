#include "doctest.h"
#include "List.h"

TEST_CASE("List grows past its initial capacity, preserving elements") {
	List<int> l;
	for (int i = 0; i < 25; i++)        // forces increaseCapacity() twice
		l.add(i);
	CHECK(l.getCount() == 25);
	for (int i = 0; i < 25; i++)
		CHECK(l[i] == i);
}

TEST_CASE("List insert shifts existing elements") {
	List<int> l;
	l.add(1);
	l.add(2);
	l.add(3);
	l.insert(1, 99);                    // -> 1, 99, 2, 3
	CHECK(l.getCount() == 4);
	CHECK(l[0] == 1);
	CHECK(l[1] == 99);
	CHECK(l[2] == 2);
	CHECK(l[3] == 3);
}
