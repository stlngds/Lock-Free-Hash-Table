// Contributors: Grace Biggs
// Adapted from Michael's "High performance dynamic lock-free hash tables and list-based sets":
// https://doi.org/10.1145/564870.564881

#include <iostream>
#include <string>
#include "LockFreeHashTable.hpp"

int main()
{
    LockFreeHashTable<int, std::string> ht;

    // Insert elements to trigger growth
    for (int i = 0; i < 200; ++i) {
        ht.insert(i, "Value" + std::to_string(i));
    }
    std::cout << "Contains 150: " << ht.contains(150) << std::endl; // 1

    // Remove elements to trigger shrink
    for (int i = 0; i < 150; ++i) {
        ht.remove(i);
    }
    std::cout << "Contains 50: " << ht.contains(50) << std::endl;  // 0
    std::cout << "Contains 175: " << ht.contains(175) << std::endl; // 1
}