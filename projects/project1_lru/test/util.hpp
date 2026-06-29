#pragma once
#include<vector>

inline bool equal_vector(const std::vector<int>& a, const std::vector<int>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    int idx = 0;
    for (int v:a) {
        if (b[idx] != v) {
            return false;
        }
        ++idx;
    }
    return true;
}