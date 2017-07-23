#include <stdio.h>
#include <stdint.h>
#include <zlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>

#include <iostream>
#include <atomic>
#include <map>
#include <set>
#include <vector>
    
#include "plog.pb.h"

#define PL_CURRENT_VERSION 2
// compression level
#define PL_Z_LEVEL 9
// 16 MB chunk
#define PL_CHUNKSIZE (1024 * 1024 * 16)
// header at most this many bytes
#define PL_HEADER_SIZE 128

typedef enum {
    PL_MODE_WRITE,
    PL_MODE_READ_FWD,
    PL_MODE_READ_BWD,
    PL_MODE_UNKNOWN
} PlMode;

typedef struct pandalog_header_struct {
    uint32_t version;     // version number
    uint64_t dir_pos;     // position in file of directory
    uint32_t chunk_size;  // chunk size
} PlHeader;

typedef struct plog_chunk {
    uint64_t start;
    uint64_t size;
} PlChunk;

typedef struct dir_entry {
    uint64_t instr;
    uint64_t start;
    uint64_t num_entries;
} DirEntry;

typedef struct pldir {
    uint32_t num_entries;
    DirEntry entries[];
} __attribute__((packed)) PlDir;

struct arg {
    int start;
    int size;
};

uint8_t *plog = NULL;
PlChunk *chunks = NULL;
PlHeader *hdr = NULL;
PlDir *dir = NULL;

std::map<uint64_t,std::set<uint64_t>> ptr_to_pc;
pthread_mutex_t tcn_insert_lock = PTHREAD_MUTEX_INITIALIZER;
std::map<uint64_t,std::vector<int>> ptr_to_label;
pthread_mutex_t maplock = PTHREAD_MUTEX_INITIALIZER;

std::atomic<int> processed(0);
int total_chunks = 0;

template<typename T>
void update_maximum(std::atomic<T>& maximum_value, T const& value) noexcept
{
    T prev_value = maximum_value;
    while(prev_value < value && !maximum_value.compare_exchange_weak(prev_value, value));
}


unsigned long decompress_chunk(uint8_t **outbuf, uint8_t *chunk, unsigned long *chunk_size) {
    unsigned long uncompressed_size = *chunk_size;
    while (true) {
        int ret = uncompress(*outbuf, &uncompressed_size, chunk, *chunk_size);
        if (ret == Z_BUF_ERROR) {
            *chunk_size *= 2;
            *outbuf = (uint8_t *) realloc(*outbuf, *chunk_size);
            uncompressed_size = *chunk_size;
        }
        else if (ret == Z_OK) {
            break;
        }
        else {
            printf("Some other zlib error, %d\n", ret);
            exit(1);
        }
    }
    return uncompressed_size;
}

void *process_chunks(void *args) {
    arg *a = (arg *)args;
    unsigned long chunk_size = hdr->chunk_size;
    uint8_t *outbuf = (uint8_t *) malloc(chunk_size);

    for (unsigned int i = a->start; i < a->start+a->size; i++) {
        PlChunk *chunk = &chunks[i];
        unsigned long uncompressed_size = 
            decompress_chunk(&outbuf, plog+chunk->start, &chunk_size);
        int j = 0;
        while (j < uncompressed_size) {
            panda::LogEntry le;
            uint32_t entry_size = *(uint32_t *)(outbuf+j);
            j += 4;
            assert(le.ParseFromString(std::string((const char *)(outbuf+j), entry_size)));
            //if (i > 0) std::cout << le.DebugString() << std::endl;
            assert(le.instr() >= dir->entries[i].instr);
            if (le.has_tainted_instr()) {
                for (auto tq : le.tainted_instr().taint_query()) {
                    //printf("(%zu, %zu, %zu) %zu %u\n", dir->entries[i].instr, dir->entries[i].start, dir->entries[i].num_entries, le.instr(), tq.tcn());
                    if (tq.has_unique_label_set()) {
                        auto uls = tq.unique_label_set();
                        pthread_mutex_lock(&maplock);
                        std::copy(
                            uls.label().begin(),
                            uls.label().end(),
                            std::back_inserter(ptr_to_label[uls.ptr()])
                        );
                        pthread_mutex_unlock(&maplock);
                    }
                    // Can we get rid of this lock in favor of an atomic?
                    pthread_mutex_lock(&tcn_insert_lock);
                    ptr_to_pc[tq.ptr()].insert(le.pc());
                    pthread_mutex_unlock(&tcn_insert_lock);

                }
            }
            j += entry_size;
        }
        processed++;
    }
    return NULL;
}

void *progress_func(void *arg) {
    while (processed != total_chunks) {
        int p = processed.load();
        printf("\r%4d / %4d chunks processed (%3.2f%%).", p, total_chunks, 100*((float)p/total_chunks));
        fflush(stdout);
        usleep(500000);
    }
    printf("\n");
    return NULL;
}

int main(int argc, char **argv) {
    int fd = open(argv[1], O_RDONLY);
    struct stat st;
    stat(argv[1], &st);
    plog = (uint8_t *) mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    //printf("Mapped at %p\n", plog);
    hdr = (PlHeader *)plog;
    printf("PandaLog v%u with directory at %" PRId64 " chunk size %x\n",
        hdr->version, hdr->dir_pos, hdr->chunk_size);
    dir = (PlDir *)(plog + hdr->dir_pos);
    //printf("Directory entries in pandalog: %u\n", dir->num_entries);
    total_chunks = dir->num_entries;
    chunks = (PlChunk *) malloc(sizeof(PlChunk) * total_chunks);
    //printf("Dumping directory:\n");
    for (int i = 0; i < total_chunks; i++) {
        chunks[i].start = dir->entries[i].start;
        if (i+1 != total_chunks)
            chunks[i].size = dir->entries[i+1].start - chunks[i].start;
        else
            chunks[i].size = hdr->dir_pos - chunks[i].start;
        //printf("  %zu - %zu\n", chunks[i].start, chunks[i].start+chunks[i].size);
    }

    int ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    pthread_t *threads = (pthread_t *) malloc(sizeof(pthread_t)*ncpu);
    arg *thread_args = (arg *) malloc(sizeof(arg)*ncpu); 
    for (int i = 0; i < ncpu; i++) {
        thread_args[i].start = (total_chunks / ncpu) * i;
        thread_args[i].size = (i < ncpu - 1) ? 
            (total_chunks / ncpu) : (total_chunks - thread_args[i].start);
        //printf("Creating thread to process Chunks %u - %u\n", thread_args[i].start, thread_args[i].start+thread_args[i].size);
        pthread_create(&threads[i], NULL, process_chunks, &thread_args[i]);
    }
    pthread_t progress_thread;
    pthread_create(&progress_thread, NULL, progress_func, NULL);
    for (int i = 0; i < ncpu; i++) {
        pthread_join(threads[i], NULL);
    }
    pthread_join(progress_thread, NULL);

    printf("Final pass to gather PCs...\n");
    std::map<int,std::set<uint64_t>> label_to_pc;
    for (auto kvp : ptr_to_label) {
        for (auto l : kvp.second) {
            label_to_pc[l].insert(ptr_to_pc[kvp.first].begin(), ptr_to_pc[kvp.first].end());
        }
    }

    FILE *f = fopen("label_pcs.txt", "w");
    for (auto kvp : label_to_pc) {
        fprintf(f, "%d", kvp.first);
        for (auto pc : kvp.second)
            fprintf(f, " %#x", pc);
        fprintf(f, "\n");
    }
    fclose(f);

    return 0;
}
