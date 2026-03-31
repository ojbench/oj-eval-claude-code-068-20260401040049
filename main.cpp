#include "allocator.hpp"
#include <iostream>
#include <string>
#include <map>

int main() {
    std::size_t poolSize;
    std::cin >> poolSize;

    TLSFAllocator allocator(poolSize);

    std::string command;
    std::map<int, void*> pointers;

    while (std::cin >> command) {
        if (command == "allocate") {
            int id;
            std::size_t size;
            std::cin >> id >> size;

            void* ptr = allocator.allocate(size);
            if (ptr) {
                pointers[id] = ptr;
                std::cout << "allocate " << id << " success" << std::endl;
            } else {
                std::cout << "allocate " << id << " fail" << std::endl;
            }
        } else if (command == "deallocate") {
            int id;
            std::cin >> id;

            if (pointers.find(id) != pointers.end()) {
                allocator.deallocate(pointers[id]);
                pointers.erase(id);
                std::cout << "deallocate " << id << " success" << std::endl;
            } else {
                std::cout << "deallocate " << id << " fail" << std::endl;
            }
        } else if (command == "max_available") {
            std::cout << allocator.getMaxAvailableBlockSize() << std::endl;
        } else if (command == "pool_size") {
            std::cout << allocator.getMemoryPoolSize() << std::endl;
        }
    }

    return 0;
}
