#include <thread>
#include <atomic>
#include <string>
#include <stdio.h>
#include <unistd.h>
#include <condition_variable>
#include <mutex>

using namespace std;

class Test{
    public:
    int *ptr = nullptr;
    int *ptr2 = nullptr;
    int num = 0;
};

int main(void){
    condition_variable cv;
    mutex m;
    atomic<Test *> p(nullptr);
    int test = 0;
    thread t([&p, &cv, &m, &test]{
        //p.exchange(new Test());
        //p.load()->ptr = new int(10);
        //p.load()->ptr2 = new int(100);
        /*for (int i = 0; i < 100; i++){
            p.load()->num++;
            *(p.load()->ptr)+=1;
            *(p.load()->ptr2)+=1;
        }
        */
        unique_lock<mutex> lock(m);
        test = 100;
        cv.notify_one();

    });
    {
        unique_lock<mutex> lock(m);
        cv.wait(lock, [&test]{return test != 0;});
    }
    //printf("%d %d %d\n", *(p.load()->ptr), *(p.load()->ptr2), p.load()->num);
    printf("%d\n", test);
    t.join();
}