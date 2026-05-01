#pragma once
#include "game_state.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

inline SharedState* shm_create() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open create"); exit(1); }
    if (ftruncate(fd, sizeof(SharedState)) < 0) { perror("ftruncate"); exit(1); }
    void* ptr = mmap(nullptr, sizeof(SharedState),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) { perror("mmap create"); exit(1); }
    SharedState* s = reinterpret_cast<SharedState*>(ptr);
    s->init();
    return s;
}

inline SharedState* shm_attach() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0) { perror("shm_open attach"); exit(1); }
    void* ptr = mmap(nullptr, sizeof(SharedState),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) { perror("mmap attach"); exit(1); }
    return reinterpret_cast<SharedState*>(ptr);
}

inline void shm_detach(SharedState* s) {
    munmap(s, sizeof(SharedState));
}

inline void shm_destroy() {
    shm_unlink(SHM_NAME);
}