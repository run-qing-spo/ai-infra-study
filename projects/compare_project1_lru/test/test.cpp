#include<iostream>
#include"lru_base.hpp"
#include"util.hpp"
#include<cassert>
#include<vector>

int testOne() {
    std::vector<int> a{1, 2, 4, 3};
    lru_base::lrucache_base* node = new lru_base::lrucache_base(1);
    for (auto u:a) {
        node->push(u);
    }
    std::vector<int> b;
    for (lru_base::lrucache_base* cur = node; cur != nullptr; cur = cur->next) {
        b.emplace_back(cur->val);
    }

    assert(equal_vector(std::vector<int> {1, 3, 4, 2, 1}, b));
    if (equal_vector(std::vector<int> {1, 3, 4, 2, 1}, b)) {
        std::cout << " [PASS] testOne" << std::endl;
    }else {
        std::cout << " [FAILED] testOne" << std::endl;
        return 1;
    }
    return 0;
}

int main() {

    return testOne();
    
    // return 0;
}