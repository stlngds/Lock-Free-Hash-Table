// Contributors: Grace Biggs
// Adapted from Michael's "High performance dynamic lock-free hash tables and list-based sets":
// https://doi.org/10.1145/564870.564881

#include <iostream>
#include "LockFreeHashTable.hpp"

int main()
{
    LockFreeHashTable<int, int> ht;
    ht.insert(42, 100);
    std::cout << "Contains 42? " << ht.contains(42) << std::endl; // 1
    ht.remove(42);
    std::cout << "Contains 42? " << ht.contains(42) << std::endl; // 0
}