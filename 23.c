#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <getopt.h>
#include <sys/stat.h>
#include <errno.h>

#define MAGIC_NUMBER 0x5844494E
#define DEFAULT_BLOCK_SIZE 4096
#define MICRODATA_SIZE 256
#define INODE_SIZE 512
#define FILENAME_MAX 224
#define MAX_COMMAND 256

typedef struct {
    uint32_t magic;
    uint32_t block_size;
    uint32_t inode_count;
    uint32_t free_inodes;
    uint32_t free_blocks;
    uint32_t inode_table;
    uint32_t bitmap_blocks;
    uint32_t root_inode;
    uint32_t l1_cache_size;
    uint32_t free_inode_hint;
    uint8_t padding[4036];
} SuperBlock;

typedef struct {
    char name[FILENAME_MAX];
    uint32_t size;
    uint32_t flags;
    time_t created;
    time_t modified;
    union {
        uint8_t micro_data[MICRODATA_SIZE];
        struct {
            uint32_t blocks[12];
            uint32_t indirect_block;
        };
    };
    uint32_t last_block;
    uint32_t access_pattern;
} Inode;

typedef struct LRUNode {
    uint32_t inode_num;
    Inode inode;
    uint8_t pinned;
    struct LRUNode* prev;
    struct LRUNode* next;
    struct LRUNode* next_hash;
} LRUNode;

typedef struct {
    LRUNode** hashmap;
    LRUNode* head;
    LRUNode* tail;
    uint32_t capacity;
    uint32_t size;
} LRUCache;

int disk_fd;
SuperBlock sb;
uint8_t* block_bitmap;
LRUCache* l1_cache;


void panic(const char* msg) {
    fprintf(stderr, "Fatal error: %s (%d)\n", msg, errno);
    exit(EXIT_FAILURE);
}

LRUCache* lru_cache_create(uint32_t capacity) {
    LRUCache* cache = malloc(sizeof(LRUCache));
    if (!cache) panic("Cache alloc failed");
    
    cache->capacity = capacity;
    cache->size = 0;
    cache->head = cache->tail = NULL;
    cache->hashmap = calloc(capacity, sizeof(LRUNode*));
    if (!cache->hashmap) panic("Hashmap alloc failed");
    
    return cache;
}

void lru_cache_free(LRUCache* cache) {
    LRUNode* current = cache->head;
    while (current) {
        LRUNode* next = current->next;
        free(current);
        current = next;
    }
    free(cache->hashmap);
    free(cache);
}

Inode* lru_cache_get(LRUCache* cache, uint32_t inode_num) {
    if (!cache || cache->capacity == 0) return NULL;

    uint32_t hash = inode_num % cache->capacity;
    LRUNode* node = cache->hashmap[hash];
    
    while (node) {
        if (node->inode_num == inode_num) {
            if (node != cache->head) {
                if (node->prev) node->prev->next = node->next;
                if (node->next) node->next->prev = node->prev;
                if (node == cache->tail) cache->tail = node->prev;
                
                node->prev = NULL;
                node->next = cache->head;
                if (cache->head) cache->head->prev = node;
                cache->head = node;
            }
            return &node->inode;
        }
        node = node->next_hash;
    }
    return NULL;
}

void lru_cache_put(LRUCache* cache, uint32_t inode_num, Inode* inode, uint8_t pinned) {
    if (!cache || cache->capacity == 0) return;

    uint32_t hash = inode_num % cache->capacity;
    LRUNode* node = cache->hashmap[hash];
    
    while (node) {
        if (node->inode_num == inode_num) {
            node->inode = *inode;
            node->pinned = pinned;
            lru_cache_get(cache, inode_num);
            return;
        }
        node = node->next_hash;
    }

    LRUNode* new_node = malloc(sizeof(LRUNode));
    if (!new_node) panic("Node alloc failed");
    
    new_node->inode_num = inode_num;
    new_node->inode = *inode;
    new_node->pinned = pinned;
    new_node->prev = NULL;
    new_node->next = cache->head;
    new_node->next_hash = cache->hashmap[hash];
    
    cache->hashmap[hash] = new_node;
    
    if (cache->head) cache->head->prev = new_node;
    cache->head = new_node;
    if (!cache->tail) cache->tail = new_node;
    
    cache->size++;

    while (cache->size > cache->capacity) {
        LRUNode* tail = cache->tail;
        while (tail && tail->pinned) {
            tail = tail->prev;
        }
        
        if (!tail) {
            fprintf(stderr, "Cache overflow with pinned nodes!\n");
            free(new_node);
            cache->size--;
            return;
        }

        if (tail->prev) tail->prev->next = NULL;
        else cache->head = NULL;
        
        cache->tail = tail->prev;
        uint32_t tail_hash = tail->inode_num % cache->capacity;
        LRUNode** ptr = &cache->hashmap[tail_hash];
        while (*ptr != tail) ptr = &(*ptr)->next_hash;
        *ptr = tail->next_hash;
        
        free(tail);
        cache->size--;
    }
}

Inode* get_inode(uint32_t inode_num) {
    Inode* cached = lru_cache_get(l1_cache, inode_num);
    if (cached) return cached;

    Inode inode;
    off_t offset = sb.inode_table * sb.block_size + inode_num * INODE_SIZE;
    if (pread(disk_fd, &inode, INODE_SIZE, offset) != INODE_SIZE)
        panic("Inode read failed");
    
    lru_cache_put(l1_cache, inode_num, &inode, 0);
    return lru_cache_get(l1_cache, inode_num);
}

void format_disk(const char* path, uint64_t size, uint32_t l1_cache_size) {
    disk_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (disk_fd < 0) panic("Disk open failed");
    
    if (ftruncate(disk_fd, size) < 0)
        panic("Disk truncate failed");

    uint32_t block_size = DEFAULT_BLOCK_SIZE;
    uint32_t total_blocks = size / block_size;
    uint32_t inode_count = total_blocks / 4;  // Исправлено
    uint32_t bitmap_size = (total_blocks + 7) / 8;
    
    sb = (SuperBlock){
        .magic = MAGIC_NUMBER,
        .block_size = block_size,
        .inode_count = inode_count,
        .free_inodes = inode_count - 1,
        .free_blocks = total_blocks - 3,
        .inode_table = 2,
        .bitmap_blocks = (bitmap_size + block_size - 1) / block_size,
        .root_inode = 0,
        .l1_cache_size = l1_cache_size,
        .free_inode_hint = 1
    };

    if (pwrite(disk_fd, &sb, sizeof(SuperBlock), 0) != sizeof(SuperBlock))
        panic("Superblock write failed");

    block_bitmap = calloc(sb.bitmap_blocks, block_size);
    if (!block_bitmap) panic("Bitmap alloc failed");
    
    for (int i = 0; i < 3; i++)
        block_bitmap[i/8] |= 1 << (i%8);
    
    if (pwrite(disk_fd, block_bitmap, sb.bitmap_blocks * block_size, block_size) != sb.bitmap_blocks * block_size)
        panic("Bitmap write failed");

    Inode* inode_table = calloc(inode_count, INODE_SIZE);
    if (!inode_table) panic("Inode table alloc failed");
    
    inode_table[0] = (Inode){
        .name = "/",
        .flags = 1,
        .created = time(NULL),
        .modified = time(NULL)
    };
    
    if (pwrite(disk_fd, inode_table, inode_count * INODE_SIZE, 2 * block_size) != inode_count * INODE_SIZE)
        panic("Inode table write failed");
    
    free(inode_table);
    close(disk_fd);
}

uint32_t allocate_block() {
    for (uint32_t i = sb.inode_table + 1; i < sb.free_blocks; i++) {
        if (!(block_bitmap[i/8] & (1 << (i%8)))) {
            block_bitmap[i/8] |= 1 << (i%8);
            sb.free_blocks--;
            
            off_t offset = sb.block_size + (i/8);
            uint8_t byte = block_bitmap[i/8];
            if (pwrite(disk_fd, &byte, 1, offset) != 1)
                panic("Bitmap update failed");
            
            return i;
        }
    }
    panic("No free blocks");
}

int find_inode(const char* filename) {
    for (uint32_t i = sb.free_inode_hint; i < sb.inode_count; i++) {
        Inode* inode = get_inode(i);
        if (strcmp(inode->name, filename) == 0) return i;
        if (inode->name[0] == '\0') {
            sb.free_inode_hint = i;
            break;
        }
    }
    
    for (uint32_t i = 1; i < sb.free_inode_hint; i++) {
        Inode* inode = get_inode(i);
        if (strcmp(inode->name, filename) == 0) return i;
    }
    return -1;
}

void write_from_buffer(const char* dst, const char* data, size_t size) {
    disk_fd = open("disk.img", O_RDWR);
    if (disk_fd < 0) panic("Disk open failed");

    if (find_inode(dst) != -1) {
        printf("File %s already exists!\n", dst);
        close(disk_fd);
        return;
    }

    int inode_num = -1;
    for (uint32_t i = sb.free_inode_hint; i < sb.inode_count; i++) {
        Inode* inode = get_inode(i);
        if (inode->name[0] == '\0') {
            inode_num = i;
            sb.free_inode_hint = i + 1;
            sb.free_inodes--;
            break;
        }
    }
    
    if (inode_num == -1) {
        for (uint32_t i = 1; i < sb.free_inode_hint; i++) {
            Inode* inode = get_inode(i);
            if (inode->name[0] == '\0') {
                inode_num = i;
                sb.free_inode_hint = i + 1;
                sb.free_inodes--;
                break;
            }
        }
    }
    
    if (inode_num == -1) panic("No free inodes");

    Inode inode;
    memset(&inode, 0, sizeof(Inode));
    strncpy(inode.name, dst, FILENAME_MAX);
    inode.size = size;
    inode.created = time(NULL);
    inode.modified = time(NULL);

    if (size <= MICRODATA_SIZE) {
        memcpy(inode.micro_data, data, size);
    } else {
        uint32_t blocks_needed = (size + sb.block_size - 1) / sb.block_size;
        for (int i = 0; i < blocks_needed; i++) {
            inode.blocks[i] = allocate_block();
            size_t write_size = (i == blocks_needed-1) ? 
                size % sb.block_size : sb.block_size;
            if (write_size == 0) write_size = sb.block_size;
            
            if (pwrite(disk_fd, data + i*sb.block_size, write_size,
                      inode.blocks[i] * sb.block_size) != write_size)
                panic("Data write failed");
        }
    }

    off_t inode_offset = sb.inode_table * sb.block_size + inode_num * INODE_SIZE;
    if (pwrite(disk_fd, &inode, INODE_SIZE, inode_offset) != INODE_SIZE)
        panic("Inode write failed");
    
    lru_cache_put(l1_cache, inode_num, &inode, 1);
    close(disk_fd);
}

void write_file(const char* dst, const char* src) {
    FILE* fp = fopen(src, "rb");
    if (!fp) {
        fprintf(stderr, "Can't open source file: %s\n", src);
        return;
    }

    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char* buffer = malloc(size);
    if (!buffer) {
        fclose(fp);
        panic("Buffer allocation failed");
    }

    fread(buffer, 1, size, fp);
    fclose(fp);

    write_from_buffer(dst, buffer, size);
    free(buffer);
}

void list_files() {
    disk_fd = open("disk.img", O_RDWR);
    if (disk_fd < 0) panic("Disk open failed");

    for (uint32_t i = 0; i < sb.inode_count; i++) {
        Inode* inode = get_inode(i);
        if (inode->name[0] != '\0') {
            printf("%-20s %8u B %s", inode->name, inode->size, ctime(&inode->created));
        }
    }
    close(disk_fd);
}

void read_file(const char* filename) {
    disk_fd = open("disk.img", O_RDWR);
    if (disk_fd < 0) panic("Disk open failed");

    int inode_num = find_inode(filename);
    if (inode_num == -1) {
        printf("File not found!\n");
        return;
    }

    Inode* inode = get_inode(inode_num);
    if (inode->size <= MICRODATA_SIZE) {
        printf("%.*s\n", inode->size, inode->micro_data);
    } else {
        uint8_t* data = malloc(inode->size);
        for (uint32_t i = 0; i < (inode->size + sb.block_size - 1)/sb.block_size; i++) {
            if (i < 12) {
                off_t offset = inode->blocks[i] * sb.block_size;
                pread(disk_fd, data + i*sb.block_size, sb.block_size, offset);
            }
        }
        printf("%.*s\n", inode->size, data);
        free(data);
    }
    close(disk_fd);
}

void mount_disk() {
    disk_fd = open("disk.img", O_RDWR);
    if (disk_fd < 0) panic("Disk open failed");
    
    if (pread(disk_fd, &sb, sizeof(SuperBlock), 0) != sizeof(SuperBlock))
        panic("Superblock read failed");
    
    block_bitmap = malloc(sb.bitmap_blocks * sb.block_size);
    if (!block_bitmap) panic("Bitmap alloc failed");
    
    if (pread(disk_fd, block_bitmap, sb.bitmap_blocks * sb.block_size, sb.block_size) != sb.bitmap_blocks * sb.block_size)
        panic("Bitmap read failed");
    
    l1_cache = lru_cache_create(sb.l1_cache_size);
    get_inode(sb.root_inode);
}


void benchmark() {
    const int num_files = 1000;
    const size_t file_size = 256; // 1KB
    char filename[FILENAME_MAX];
    char data[file_size];
    struct timespec start, end;
    double total_time;
    
    // Генерируем тестовые данные
    int urandom = open("/dev/urandom", O_RDONLY);
    if (urandom < 0 || read(urandom, data, file_size) != file_size) {
        panic("Failed to generate test data");
    }
    close(urandom);

    printf("Starting benchmark: %d files of 1KB each\n", num_files);
    
    // Замер времени начала
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        panic("Clock error");
    }

    // Создаем файлы
    for (int i = 0; i < num_files; i++) {
        snprintf(filename, FILENAME_MAX, "bench_%08d.dat", i);
        write_from_buffer(filename, (char*)data, file_size);
    }

    // Замер времени окончания
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        panic("Clock error");
    }

    // Вычисляем результаты
    total_time = (end.tv_sec - start.tv_sec) + 
                (end.tv_nsec - start.tv_nsec) / 1e9;
    
    double files_per_sec = num_files / total_time;
    double mb_per_sec = (num_files * file_size) / (1024.0 * 1024.0) / total_time;

    printf("\nBenchmark results:\n");
    printf("Total files:     %d\n", num_files);
    printf("Total time:      %.3f seconds\n", total_time);
    printf("Files per second: %.2f\n", files_per_sec);
    printf("Throughput:      %.2f MB/s\n", mb_per_sec);
}

void start_shell() {
    char command[MAX_COMMAND];
    char arg1[MAX_COMMAND];
    char arg2[MAX_COMMAND];

    printf("Inode-X Interactive Shell\n");
    while(1) {
        printf("\nInode-X> ");
        if (!fgets(command, MAX_COMMAND, stdin)) break;

        if (sscanf(command, "create %s %s", arg1, arg2) == 2) {
            write_file(arg1, arg2);
            int inode_num = find_inode(arg1);
            if (inode_num != -1) {
                Inode* inode = get_inode(inode_num);
                lru_cache_put(l1_cache, inode_num, inode, 1);
            }
        }
        else if (strncmp(command, "benchmark", 9) == 0) {
            //mount_disk();
            benchmark();
        }
        //else if (sscanf(command, "echo %s \"%[^\"]", arg1, arg2) == 2) {
        else if (sscanf(command, "echo %s %s", arg1, arg2) == 2) {
            write_from_buffer(arg1, arg2, strlen(arg2));
        }
        else if (sscanf(command, "read %s", arg1) == 1) {
            read_file(arg1);
        }
        else if (sscanf(command, "pin %s", arg1) == 1) {
            int inode_num = find_inode(arg1);
            if (inode_num != -1) {
                Inode* inode = get_inode(inode_num);
                lru_cache_put(l1_cache, inode_num, inode, 1);
                printf("Inode %d pinned\n", inode_num);
            }
        }
        else if (strncmp(command, "list", 4) == 0) {
            list_files();
        }
        else if (strncmp(command, "exit", 4) == 0) {
            break;
        }
        else {
            printf("Commands:\n"
                   "create <dst> <src> - Write file\n"
                   "echo <file> \"text\" - Write text\n"
                   "read <file>        - Read file\n"
                   "pin <file>         - Pin inode\n"
                   "list               - List files\n"
                   "exit               - Exit\n");
        }
    }
    
    lru_cache_free(l1_cache);
    free(block_bitmap);
    close(disk_fd);
}

int main(int argc, char* argv[]) {
    uint64_t format_size = 0;
    uint32_t l1_cache_size = 128;
    int opt;

    while ((opt = getopt(argc, argv, "f:k:")) != -1) {
        switch (opt) {
            case 'f':
                format_size = atoll(optarg) * 1024 * 1024;
                break;
            case 'k':
                l1_cache_size = atoi(optarg);
                break;
            default:
                fprintf(stderr, "Usage: %s -f <sizeMB> -k <cache_size>\n", argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (format_size > 0) {
        format_disk("disk.img", format_size, l1_cache_size);
        printf("Formatted disk with %luMB, cache size: %u\n", 
               format_size/(1024*1024), l1_cache_size);
    }

    if (argc == 1 || optind == argc) {
        mount_disk();
        start_shell();
    }

    return 0;
}
