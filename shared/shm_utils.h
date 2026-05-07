#pragma once
#include "shared/game_state.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

// System V shared memory key (chosen to avoid collision)
constexpr key_t SHM_KEY = 0x43524946;   // 'CRIF' in hex

inline SharedState* shm_create() {
    // First, try to clean up any existing segment
    int old_shmid = shmget(SHM_KEY, 0, 0666);
    if (old_shmid >= 0) {
        fprintf(stderr, "[SHM] Cleaning up old segment %d\n", old_shmid);
        shmctl(old_shmid, IPC_RMID, nullptr);
    }
    
    // Create new System V SHM segment
    int shmid = shmget(SHM_KEY, sizeof(SharedState), IPC_CREAT | IPC_EXCL | 0666);
    if (shmid < 0) {
        perror("[SHM] shmget create failed");
        exit(1);
    }
    
    fprintf(stderr, "[SHM] Created segment ID: %d, size: %zu bytes\n", shmid, sizeof(SharedState));
    
    void* ptr = shmat(shmid, nullptr, 0);
    if (ptr == (void*)-1) {
        perror("[SHM] shmat create failed");
        shmctl(shmid, IPC_RMID, nullptr);
        exit(1);
    }
    
    SharedState* s = reinterpret_cast<SharedState*>(ptr);
    memset(s, 0, sizeof(SharedState));
    s->init();
    
    fprintf(stderr, "[SHM] Initialized shared state at %p\n", (void*)s);
    return s;
}

inline SharedState* shm_attach() {
    // Wait briefly for segment to be created
    int shmid = -1;
    for (int retry = 0; retry < 10; ++retry) {
        shmid = shmget(SHM_KEY, sizeof(SharedState), 0666);
        if (shmid >= 0) break;
        usleep(100000);  // 100ms
    }
    
    if (shmid < 0) {
        fprintf(stderr, "[SHM] shmget attach failed after retries: %s\n", strerror(errno));
        exit(1);
    }
    
    void* ptr = shmat(shmid, nullptr, 0);
    if (ptr == (void*)-1) {
        perror("[SHM] shmat attach failed");
        exit(1);
    }
    
    fprintf(stderr, "[SHM] Attached to segment ID: %d at %p\n", shmid, ptr);
    return reinterpret_cast<SharedState*>(ptr);
}

inline void shm_detach(SharedState* s) {
    if (s) {
        fprintf(stderr, "[SHM] Detaching from %p\n", (void*)s);
        shmdt(reinterpret_cast<void*>(s));
    }
}

inline void shm_destroy() {
    int shmid = shmget(SHM_KEY, 0, 0666);
    if (shmid >= 0) {
        fprintf(stderr, "[SHM] Destroying segment ID: %d\n", shmid);
        shmctl(shmid, IPC_RMID, nullptr);
    }
}