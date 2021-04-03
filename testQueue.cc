#include "CircleQueue.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    CircleQueue<int> q(10);
    for (int i = 0; i < 100; i++){
        printf("push %d\n", i);
        q.push_back(i);
    }
    while (!q.empty()){
        int val = q.front();
        printf("%d ", val);
        q.pop_front();
    }
    printf("\n");
    while (!q.empty()){
        printf("error\n");
        q.pop_front();
    }
    bool ret = q.pop_front();
    assert(ret == false);
    for (int i = 0; i < 100; i++){
        if (!q.full()){
            printf("push %d\n", i);
            q.push_back(i);
        }
        else if (i % 2 == 1) {
            printf("pop %d\n", q.front());
            q.pop_front();
        }
        else {
        }
    }
    while (!q.empty()){
        int val = q.front();
        printf("%d ", val);
        q.pop_front();
    }
    printf("\n");
    
}