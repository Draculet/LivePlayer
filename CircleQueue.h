#include <stdio.h>

template <typename T>
class CircleQueue{
    public:
    //注意cap == capcity + 1才能同时存在capcity个元素在队列中而不用调整队列头
    CircleQueue(size_t capcity = 8)
        :cap(capcity + 1), head(0), tail(0){
        buffer = new T[cap];
    }

    ~CircleQueue(){
        if (buffer) delete[]buffer;
        buffer = nullptr;
    }


    void push_back(T &val){
        buffer[tail] = val;
        tail = (tail + 1) % cap;
        if (head == tail){
            //printf("update head %d tail %d\n", (head + 1) % cap, tail);
            head = (head + 1) % cap;
        }
    }
    bool empty(){
        return head == tail;
    }

    bool full(){
        return (tail + 1) % cap == head;
    }

    bool pop_front(){
        if (head == tail) return false;
        else head = (head + 1) % cap;
    }
    
    T &front(){
        return buffer[head];
    }

    T &back(){
        return tail == 0 ? buffer[cap - 1]: buffer[tail - 1];
    }

    private:
    size_t cap = 0;
    size_t head = 0;
    size_t tail = 0;
    T *buffer = nullptr;
};
