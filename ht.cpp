#include "ht.h"
#include <iostream>
#include <unordered_set>

void test_really_basic()
{
        cout << __func__ << endl;
        
        hash_set<int> s;
        
        assert(s.size() == 0);
        assert(s.begin() == s.end());
        assert(s.find(1) == s.end());
        
        s.insert(1);
        assert(s.size() == 1);
        assert(s.load() > 0.0);
        assert(s.begin() != s.end());

        auto it = s.find(1);        
        assert(it != s.end());
        assert(*it == 1);
        
        auto it_copy = it;
        assert(it_copy == it);
        assert(!(it_copy != it));

        ++it_copy;
        assert(it_copy == s.end());
        assert(it_copy != it);
        assert(!(it_copy == it));

        s.erase(1);
        assert(s.size() == 0);

        it = s.find(1);
        assert(it == s.end());
        assert(s.begin() == s.end());
}

void test_basic()
{
        cout << __func__ << endl;
        
        hash_set<int> s;

        for (size_t i = 0; i < 20; ++i) {
                assert(s.find(rand()) == s.end());
        }

        unordered_set<int> ctrl;
        
        for (size_t i = 0; i < 1000; ++i) {
                int val = rand();
                assert((ctrl.find(val) == ctrl.end()) == (s.find(val) == s.end()));

                s.insert(val);
                ctrl.insert(val);
                assert(s.size() == ctrl.size());

                for (const auto i : ctrl) {
                        assert(s.find(i) != s.end());
                }

                for (const auto i : s) {
                        assert(ctrl.find(i) != ctrl.end());
                }
        }
        
        const auto ctrl_copy = ctrl;
        unordered_set<int> erased;
        for (const auto i : ctrl_copy) {
                
                ctrl.erase(i);
                s.erase(i);
                erased.insert(i);

                assert(s.find(i) == s.end());
                assert(s.size() == ctrl.size());

                for (const auto j : ctrl) {
                        assert(s.find(j) != s.end());
                }

                for (const auto j : s) {
                        assert(ctrl.find(j) != ctrl.end());
                }

                for (const auto j : erased) {
                        assert(s.find(j) == s.end());
                }
        }

        assert(s.size() == 0);
}

void test_iter()
{
        cout << __func__ << endl;

        unordered_set<int> ctrl;
        hash_set<int> s;

        for (size_t i = 0; i < 1000; ++i) {
                int val = rand();
                ctrl.insert(val);
                s.insert(val);

                for (const auto v : s) {
                        assert(s.find(v) != s.end());
                        assert(ctrl.find(v) != ctrl.end());
                }
        }
        
        const auto ctrl_copy = ctrl;
        for (const auto v : ctrl_copy) {
                ctrl.erase(v);
                s.erase(v);

                assert(s.find(v) == s.end());

                for (const auto v : s) {
                        assert(ctrl.find(v) != ctrl.end());
                }
        }
}

int main(int argc, char ** argv)
{
        (void)argc;
        (void)argv;

        time_t t = time(NULL);
        cout << "srand seed is " << t << endl;
        srand(t);

        test_really_basic();
        test_basic();
        test_iter();
}
