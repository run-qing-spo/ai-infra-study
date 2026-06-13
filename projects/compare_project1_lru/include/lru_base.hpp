#pragma once

namespace lru_base {
    class lrucache_base {
        public:
            explicit lrucache_base(int x):val(x), next(nullptr){}
            void push(int x) {
                lrucache_base* a = new lrucache_base(x);
                a->next = next;
                next = a;
            }
        // private:
            int val;
            lrucache_base* next;
    };
}