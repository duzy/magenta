// Copyright 2016 The Fuchsia Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* name;
    int (*func)(volatile unsigned int*);
    const char* desc;
} command_t;

int blind_write(volatile unsigned int* addr) {
    *addr = 0xBAD1DEA;
    return 0;
}

int blind_read(volatile unsigned int* addr) {
    return (int)(*addr);
}

int ro_write(volatile unsigned int* addr) {
    // test that we cannot write to RO code memory
    volatile unsigned int* p = (volatile unsigned int*)&ro_write;
    *p = 99;
    return 0;
}

int nx_run(volatile unsigned int* addr) {
    // test that we cannot execute NX memory
    static uint8_t codebuf[16];
    void (*func)(void) = (void*)codebuf;
    func();
    return 0;
}

// Note that as of 5/21/16 the crash reads:
// PageFault:199: UNIMPLEMENTED: faulting with a page already present.
int stack_overflow(volatile unsigned int* i_array) {
    volatile unsigned int array[512];
    if (i_array) {
        array[0] = i_array[0] + 1;
        if (array[0] < 4096)
            return stack_overflow(array);
    } else {
        array[0] = 0;
        return stack_overflow(array);
    }
    return 0;
}

int undefined(volatile unsigned int* unused) {
#if defined(__x86_64__)
    __asm__ volatile("ud2");
#elif defined(__aarch64__)
    __asm__ volatile("brk #0"); // not undefined, but close enough
#elif defined(__arm__)
    __asm__ volatile("udf");
#else
#error "need to define undefined for this architecture"
#endif
    return 0;
}

command_t commands[] = {
    {"write0", blind_write, "write to address 0x0"},
    {"read0", blind_read, "read address 0x0"},
    {"writero", ro_write, "write to read only code segment"},
    {"stackov", stack_overflow, "overflow the stack (recursive)"},
    {"und", undefined, "undefined instruction"},
    {"nx_run", nx_run, "run in no-execute memory"},
    {NULL, NULL, NULL}};

int main(int argc, char** argv) {
    printf("=@ crasher @=\n");

    if (argc < 2) {
        printf("default to write0  (use 'help' for more options).\n");
        blind_write(NULL);
    } else {
        if (strcmp("help", argv[1])) {
            for (command_t* cmd = commands; cmd->name != NULL; ++cmd) {
                if (strcmp(cmd->name, argv[1]) == 0) {
                    printf("doing : %s\n", cmd->desc);
                    cmd->func(NULL);
                    goto exit; // should not reach here.
                }
            }
        }

        printf("known commands are:\n");
        for (command_t* cmd = commands; cmd->name != NULL; ++cmd) {
            printf("%s : %s\n", cmd->name, cmd->desc);
        }
        return 0;
    }

exit:
    printf("crasher: exiting normally ?!!\n");
    return 0;
}
