#include<stdlib.h>
#include<stdio.h>
#include<omp.h>
#include<string.h>
#include<ctype.h>
#include<omp.h>

#define BUS_SIZE 1024

enum State{
    INVALID, SHARED, EXCLUSIVE, MODIFIED
};

enum Bus{
    REQUEST, FLUSHED, INVALIDATE, VALUE
};

typedef char byte;
typedef enum State State;

struct cache {
    byte address; // This is the address in memory.
    byte value; // This is the value stored in cached memory.
    // State for you to implement MESI protocol.
    State state;
};

struct decoded_inst {
    int type; // 0 is RD, 1 is WR
    byte address;
    byte value; // Only used for WR 
};

struct bus{
    byte bus[BUS_SIZE][3];
    int top;
};


typedef struct bus bus;
typedef struct cache cache;
typedef struct decoded_inst decoded;

void put_on_bus(int x, int y, int z, omp_lock_t* lock, bus* bus){
    omp_set_lock(lock);
    bus->bus[bus->top][0] = x;
    bus->bus[bus->top][1] = y;
    bus->bus[bus->top][2] = z;
    bus->top++;
    omp_unset_lock(lock);
}

void put_on_bus_with_plock(int x, int y, int z, bus* bus){

    bus->bus[bus->top][0] = x;
    bus->bus[bus->top][1] = y;
    bus->bus[bus->top][2] = z;
    bus->top++;

}


/*
 * This is a very basic C cache simulator.
 * The input files for each "Core" must be named core_1.txt, core_2.txt, core_3.txt ... core_n.txt
 * Input files consist of the following instructions:
 * - RD <address>
 * - WR <address> <val>
 */


// Decode instruction lines
decoded decode_inst_line(char * buffer){
    decoded inst;
    char inst_type[3];
    sscanf(buffer, "%s", inst_type);
    if(!strcmp(inst_type, "RD")){
        inst.type = 0;
        int addr = 0;
        sscanf(buffer, "%s %d", inst_type, &addr);
        inst.value = -1;
        inst.address = addr;
    } else if(!strcmp(inst_type, "WR")){
        inst.type = 1;
        int addr = 0;
        int val = 0;
        sscanf(buffer, "%s %d %d", inst_type, &addr, &val);
        inst.address = addr;
        inst.value = val;
    }
    return inst;
}

// Helper function to print the cachelines
void print_cachelines(cache * c, int cache_size){
    for(int i = 0; i < cache_size; i++){
        cache cacheline = *(c+i);
        printf("Address: %d, State: %d, Value: %d\n", cacheline.address, cacheline.state, cacheline.value);
    }
}

void wait_for_val(byte address, int s_idx, bus* bus, omp_lock_t* lb, byte* memory, omp_lock_t* lm, cache* c){
    int flag = 0;
    while(!flag){
        int i;
        //printf("Outer loop\n");
        omp_set_lock(lb);
        for(i=s_idx; i<bus->top; i++){
            //printf("Checking in mem %d %d\n", s_idx, bus->top);
            if(bus->bus[i][0] == VALUE && bus->bus[i][1] == address){
                c->value = bus->bus[i][2];
                c->state = SHARED;
                flag = 1;
            }
            else if(bus->bus[i][0] == FLUSHED && bus->bus[i][1] == address){
                omp_set_lock(lm);
                c->value = memory[address];
                c->state = EXCLUSIVE;
                flag = 1;
                omp_unset_lock(lm);
            }
        }
        s_idx = i;
        omp_unset_lock(lb);
    }
}

void read_miss(int id, cache* c, byte address, bus* bus, byte* memory, omp_lock_t* lb, omp_lock_t* lm){
    int t = bus->top-1;
    
    omp_set_lock(lb);
    while(t>=0){
        if(bus->bus[t][1] == address && bus->bus[t][0] != REQUEST) break;
        else t--;
    }
    c->address = address;
    omp_unset_lock(lb);
    if(t >= 0 && bus->bus[t][0] != FLUSHED){
        // printf("Request on bus %d\n", address);
        put_on_bus(REQUEST, address, id, lb, bus);
        wait_for_val(address, bus->top-1, bus, lb, memory, lm, c);
        
    }
    else{
        omp_set_lock(lm);
        c->value = memory[address];
        c->state = EXCLUSIVE;
        omp_unset_lock(lm);
    }
}

void write_miss(int id, cache* c, decoded* inst, bus* bus, omp_lock_t* lb){
    c->address = inst->address;
    c->value = inst->value;
    c->state = MODIFIED;
    // printf("INVALIDATE on bus %d %d\n", inst->address, id);
    put_on_bus(INVALIDATE, inst->address, id, lb, bus);
}

void write_hit(int id, cache* c, decoded* inst, bus* bus, omp_lock_t* lb){
    switch(c->state){
        case MODIFIED:
            c->value = inst->value;
            break;
        case EXCLUSIVE:
            c->value = inst->value;
            c->state = MODIFIED;
            break;
        case SHARED:
            c->value = inst->value;
            c->state = MODIFIED;
            // printf("INVALIDATE on hit on bus\n", inst->address);
            put_on_bus(INVALIDATE, inst->address, id, lb, bus);
            break;
    }
}

void check_bus(int id, int* last_index, bus* bus, omp_lock_t* lb, cache* caches, int cache_size){
    int top = bus->top;
    int i=(*last_index)+1;
    //printf("called for %d\n", id);
    while(i<top){
        //printf("bus %d %d %d %d %d %d\n", bus->bus[i][0], bus->bus[i][1], bus->bus[i][2], id, i, *last_index);
        if(bus->bus[i][0] == VALUE || (bus->bus[i][2] == id && bus->bus[i][0] != INVALIDATE)){
            //printf("continuing\n");
            i++;
            continue;
        }
        cache* cacheline = caches + (bus->bus[i][1] % cache_size);
        // printf("bus2 %d %d %d %d %d %d\n", bus->bus[i][0], bus->bus[i][1], bus->bus[i][2], cacheline->state, id, i);
        if(bus->bus[i][0] == REQUEST && cacheline->state == MODIFIED && cacheline->address == bus->bus[i][1]){
            //printf("Request %d %d\n", id, i);
            put_on_bus(VALUE, bus->bus[i][1], cacheline->value, lb, bus);
        }
        else if(bus->bus[i][0] == INVALIDATE && cacheline->address == bus->bus[i][1]){
            if(bus->bus[i][2] != id){
                // printf("Invalidating %d %d %d %d %d %d\n", bus->bus[i][1], id, bus->bus[i][0], bus->bus[i][1], bus->bus[i][2], i);
                cacheline->state = INVALID;
            }
            else{
                // printf("self invalidation %d %d\n", id, bus->bus[i][1]);
                cacheline->state = MODIFIED;
            }
        }
        i++;
        top = bus->top;
    }
    *last_index = i-1; 
}

// This function implements the mock CPU loop that reads and writes data.
void cpu_loop(int num_threads, byte* memory){
    bus* cpu_bus = (bus*)malloc(sizeof(bus));
    cpu_bus->top = 0;

    omp_lock_t lock_b;
    omp_init_lock(&lock_b);
    
    omp_lock_t lock_m;
    omp_init_lock(&lock_m);

    omp_set_num_threads(num_threads);
    #pragma omp parallel shared(memory)
    {
        // Initialize a CPU level cache that holds about 2 bytes of data.
        int cache_size = 2;
        cache * c = (cache *) malloc(sizeof(cache) * cache_size);
        for(int i=0; i<cache_size;i++){
            c[i].state = INVALID;
        }
        // Read Input file
        int id = omp_get_thread_num();
        // printf("%s", str_id);
        char filename[1024];
        snprintf(filename, 1024, "input_%d.txt\0", id);
        FILE * inst_file = fopen(filename, "r");
        char inst_line[20];
        // Decode instructions and execute them.
        int index_checked = -1; //index of the buffer checked
        while (fgets(inst_line, sizeof(inst_line), inst_file)){
            //take action to check bus since last index_checked
            // printf("%s\t%d\n", inst_line, id);
            decoded inst = decode_inst_line(inst_line);
            /*
            * Cache Replacement Algorithm
            */
            //printf("%d\n", inst.type);
            check_bus(id, &index_checked, cpu_bus, &lock_b, c, cache_size);
            int hash = inst.address%cache_size;
            cache* cacheline = (c+hash);
            /*
            * This is where you will implement the coherancy check.
            * For now, we will simply grab the latest data from memory.
            */
            // printf("cache state %d %d %d\n", id, cacheline->state, index_checked);
            if(cacheline->address != inst.address || (cacheline->address == inst.address && cacheline->state == INVALID)){
                // Flush current cacheline to memory
                if(cacheline->state == MODIFIED){
                    *(memory + cacheline->address) = cacheline->value;
                    put_on_bus(FLUSHED, cacheline->address, id, &lock_b, cpu_bus);
                    // printf("Putting Flushed on bus %d\n", id);
                }
                

                
                if(inst.type == 0){
                    // printf("Read Miss %d\n", id);
                    read_miss(id, cacheline, inst.address, cpu_bus, memory, &lock_b, &lock_m);
                }
                if(inst.type == 1){
                    // printf("write miss %d\n", id);
                    write_miss(id, cacheline, &inst, cpu_bus, &lock_b);
                }
                //*(c+hash) = cacheline;
            }
            else{
                if(inst.type == 1){
                    // printf("Write hit %d\n", id);
                    write_hit(id, cacheline, &inst, cpu_bus, &lock_b);
                }
            }
            switch(inst.type){
                case 0:
                    printf("Reading from address %d: %d\n", cacheline->address, cacheline->value);
                    break;
                
                case 1:
                    printf("Writing to address %d: %d\n", cacheline->address, cacheline->value);
                    break;
            }
            // printf("cacheline2 state %d %d\n", id, cacheline->state);
        }
        free(c);
    }
}

int main(int c, char * argv[]){
    // Initialize Global memory
    // Let's assume the memory module holds about 24 bytes of data.
    int memory_size = 24;
    int num_threads = 2;
    byte* memory = (byte *) malloc(sizeof(byte) * memory_size);
    for(int i=0; i<memory_size; i++){
        memory[i] = 0;
    }
    cpu_loop(num_threads, memory);
    free(memory);
}