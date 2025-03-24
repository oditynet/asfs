#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <getopt.h>
#include <sys/stat.h>
//#include <errno.h> // Добавляем для обработки ошибок
#define MAX_NAME_LEN 224
#define MAX_SNAPSHOTS 32
#define BLOCK_SIZE 4096
#define MAGIC_NUMBER 0x46534653
#define DEBUG 1
#define DEVICE_PATH "image.img"

static uint32_t next_snap_id = 1; // Статический счетчик ID снапшотов

typedef struct {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t free_inodes;
    uint32_t free_blocks;
    uint32_t first_data_block;
    uint32_t block_size;
    uint32_t snapshot_count;

  uint32_t next_snap_id;
} SuperBlock;
typedef struct {
    uint32_t number;
    uint32_t snapshot_count; // Добавляем счетчик снапшотов
    uint32_t size;
    uint32_t blocks[12];
    char name[MAX_NAME_LEN];
    uint8_t used;
    time_t created;
    time_t modified;
    uint32_t snapshot_id;
    uint8_t padding[12];
    uint32_t snapshot_parent;
    uint8_t is_snapshot;
    uint8_t type; // 0 - файл, 1 - директория
} Inode;
typedef struct {
    char snapshot_name[MAX_NAME_LEN];
    uint32_t snap_id;       // Уникальный ID снапшота
    Inode inode;
    uint8_t* data;
    time_t timestamp;

    uint32_t original_inode; // Исходный inode
    uint32_t snapshot_inode; // Inode снапшота

   // uint32_t original_inode;
} Snapshot;
int disk_fd;
SuperBlock sb;
Snapshot snapshots[MAX_SNAPSHOTS];
uint8_t* block_bitmap;
uint8_t* inode_bitmap;

// Прототипы функций
uint32_t allocate_block();
void free_blocks(uint32_t* blocks, size_t count);
uint32_t find_inode(const char* filename);
void update_bitmaps();
void load_metadata();
void save_metadata();
void list_snapshots();
// Реализация недостающих функций
void list_files() {
    disk_fd = open(DEVICE_PATH, O_RDONLY);
    load_metadata();
    printf("\n%-20s %-10s %-10s %-10s %-10s %-10s %-10s\n",
           "Name", "Type", "Size", "Created", "Modified", "Inode", "Snapshot_id");
    printf("==============================================================\n");
    // Всегда показываем корневой каталог
    Inode root;
    lseek(disk_fd, sizeof(SuperBlock), SEEK_SET);
    read(disk_fd, &root, sizeof(Inode));

    char created_str[20], modified_str[20];
    strftime(created_str, 20, "%Y-%m-%d %H:%M:%S", localtime(&root.created));
    strftime(modified_str, 20, "%Y-%m-%d %H:%M:%S", localtime(&root.modified));

    printf("%-20s %-10s %u  %-10s %-10s\n",
           root.name,
           "DIR",
           0,
           created_str,
           modified_str);
    // Обработка остальных inodes
    for (uint32_t i = 1; i < sb.inode_count; i++) {
        if (!(inode_bitmap[i/8] & (1 << (i%8)))) continue;

        Inode node;
        lseek(disk_fd, sizeof(SuperBlock) + i * sizeof(Inode), SEEK_SET);
        read(disk_fd, &node, sizeof(Inode));

        if (!node.used) continue;
        strftime(created_str, 20, "%Y-%m-%d %H:%M:%S", localtime(&node.created));
        strftime(modified_str, 20, "%Y-%m-%d %H:%M:%S",
                node.modified ? localtime(&node.modified) : localtime(&node.created));
        uint32_t inode_num = find_inode(node.name);
        printf("%-20s %-10s %u  %-10s %-10s %u %u \n",
               node.name,
               node.type ? "DIR" : "FILE",
               node.size,
               created_str,
               modified_str,inode_num,node.snapshot_id);
    }
    close(disk_fd);
}
void delete_file(const char* filename) {
    disk_fd = open(DEVICE_PATH, O_RDWR);
    load_metadata();
    uint32_t inode_num = find_inode(filename);
    if (inode_num == (uint32_t)-1) {
        printf("File not found\n");
        close(disk_fd);
        return;
    }
    Inode node;
    lseek(disk_fd, sizeof(SuperBlock) + inode_num * sizeof(Inode), SEEK_SET);
    read(disk_fd, &node, sizeof(Inode));
    // Free blocks
    uint32_t blocks_count = (node.size + sb.block_size - 1) / sb.block_size;
    free_blocks(node.blocks, blocks_count);
    // Free inode
    inode_bitmap[inode_num/8] &= ~(1 << (inode_num%8));
    sb.free_inodes++;

    save_metadata();
    close(disk_fd);
    printf("File '%s' deleted\n", filename);
}
void edit_file(const char* filename, const void* new_data) {
    size_t new_size = strlen(new_data);
    disk_fd = open(DEVICE_PATH, O_RDWR);
    load_metadata();
    uint32_t inode_num = find_inode(filename);
    if (inode_num == (uint32_t)-1) {
        printf("File not found\n");
        close(disk_fd);
        return;
    }
    Inode node;
    lseek(disk_fd, sizeof(SuperBlock) + inode_num * sizeof(Inode), SEEK_SET);
    read(disk_fd, &node, sizeof(Inode));
    // Handle snapshots
//    if (node.snapshot_id != 0) {
//        create_snapshot(filename, "auto_snapshot");
//        node.snapshot_id = 0;
//    }
    uint32_t old_blocks = (node.size + sb.block_size - 1) / sb.block_size;
    uint32_t new_blocks = (new_size + sb.block_size - 1) / sb.block_size;
    // Free excess blocks
    for (int i = new_blocks; i < old_blocks; i++) {
        if (node.blocks[i] == 0) continue;
        uint32_t byte = node.blocks[i] / 8;
        uint8_t bit = 1 << (node.blocks[i] % 8);
        block_bitmap[byte] &= ~bit;
        sb.free_blocks++;
        node.blocks[i] = 0;
    }
    // Allocate new blocks
    for (int i = old_blocks; i < new_blocks; i++) {
        node.blocks[i] = allocate_block();
        if (!node.blocks[i]) {
            printf("Not enough space\n");
            free_blocks(node.blocks, i);
            close(disk_fd);
            return;
        }
    }
    // Write new data
    for (int i = 0; i < new_blocks; i++) {
        lseek(disk_fd, node.blocks[i] * sb.block_size, SEEK_SET);
        size_t write_size = (new_size - (i * sb.block_size)) > sb.block_size
                          ? sb.block_size : (new_size - (i * sb.block_size));
        ssize_t bytes_written = write(disk_fd, (char*)new_data + i*sb.block_size, write_size);
        if (bytes_written != write_size) {
		perror("[ERROR] Write failed");
    	   return;
	}
    }
    // Update inode
    node.size = new_size;
    node.modified = time(0);
    lseek(disk_fd, sizeof(SuperBlock) + inode_num * sizeof(Inode), SEEK_SET);
    ssize_t bytes_written = write(disk_fd, &node, sizeof(Inode));
    if (bytes_written !=  sizeof(Inode)) {
	perror("[ERROR] Write failed");
        return;
    }
    save_metadata();
    close(disk_fd);
    printf("File '%s' updated\n", filename);
}
uint32_t find_free_inode() {
    for (uint32_t i = 1; i < sb.inode_count; i++) { // Начинаем с 1
        uint32_t byte = i / 8;
        uint8_t bit = 1 << (i % 8);
        if (!(inode_bitmap[byte] & bit)) {
            if (DEBUG) printf("[DEBUG] Found free inode: %u\n", i);
            return i;
        }
    }
    return (uint32_t)-1;
}
void print_fs_info() {
    disk_fd = open(DEVICE_PATH, O_RDONLY);
    load_metadata();
    printf("\nFile System Information:\n");
    printf("===============================\n");
    printf("Block size:         %u bytes\n", sb.block_size);
    printf("Total blocks:       %u\n", sb.total_blocks);
    printf("Free blocks:        %u (%.1f%%)\n",
          sb.free_blocks,
          100.0 * sb.free_blocks / sb.total_blocks);
    printf("Total inodes:       %u\n", sb.inode_count);
    printf("Free inodes:        %u (%.1f%%)\n",
          sb.free_inodes,
          100.0 * sb.free_inodes / sb.inode_count);
    printf("Snapshots count:    %u\n", sb.snapshot_count);
    printf("First data block:   %u\n", sb.first_data_block);
    printf("Magic number:       0x%08X\n", sb.magic);
    printf("===============================\n");

    close(disk_fd);
}
void format_disk(int zero_fill, uint32_t block_size) {
    struct stat dev_stat;
    disk_fd = open(DEVICE_PATH, O_RDWR|O_CREAT, 0644);
    if (disk_fd < 0){
    	printf("Error open file for formating...\n");
    	return;
    }
    //temp!!!!
    /*if (ftruncate(disk_fd, 1024 * 1024) == -1) {
        perror("Ошибка установки размера файла");
        close(disk_fd);
        return;
    }*/

    fstat(disk_fd, &dev_stat);
    if (block_size % 512 != 0 || block_size < 512) {
        printf("Invalid block size (must be multiple of 512)\n");
        exit(1);
    }
    sb.block_size = block_size;
    sb.total_blocks = dev_stat.st_size / block_size;
    //sb.total_blocks = dev_stat.st_size
    sb.inode_count = sb.total_blocks / 16;
    sb.first_data_block = 3 + (sb.inode_count * sizeof(Inode)) / block_size;
    sb.free_blocks = sb.total_blocks - sb.first_data_block;
    sb.free_inodes = sb.inode_count - 1;
    sb.magic = MAGIC_NUMBER;
    if (DEBUG) {
        printf("[DEBUG] Formatting parameters:\n"
               "Block size: %u\n"
               "Total blocks: %u\n"
               "Inodes: %u\n"
               "First data block: %u\n",
               sb.block_size, sb.total_blocks,
               sb.inode_count, sb.first_data_block);
    }
    if (zero_fill) {
        uint8_t *zero = calloc(1, block_size);
    	//uint8_t *zero=0;
        for (uint32_t i = 0; i < sb.total_blocks; i++) {
            write(disk_fd, zero, block_size);
        }
        free(zero);
    }
    Inode root = {0};
    root.used = 1;
    strcpy(root.name, "/");
    root.created = time(0);
    root.type = 1; // Директория
    lseek(disk_fd, 0, SEEK_SET);
    if (0 > write(disk_fd, &sb, sizeof(SuperBlock)))
    {
    	printf("Error write superblock\n");
    	return;
    }
    if (0 > write(disk_fd, &root, sizeof(Inode)))
    {
		printf("Error write superblock\n");
		return;
	}
    printf("Device formatted with %u byte blocks\n", block_size);
    close(disk_fd);
}
void delete_snapshot(const char* snap_name) {
    disk_fd = open(DEVICE_PATH, O_RDWR);
    load_metadata();

    int found_index = -1;
    Snapshot target_snap;

    // Поиск снапшота по имени
    for (int i = 0; i < sb.snapshot_count; i++) {
        if (strcmp(snapshots[i].snapshot_name, snap_name) == 0) {
            found_index = i;
            target_snap = snapshots[i];
            break;
        }
    }

    if (found_index == -1) {
        printf("Snapshot '%s' not found\n", snap_name);
        close(disk_fd);
        return;
    }

    // 1. Освобождаем inode снапшота
    Inode snap_inode;
    lseek(disk_fd, sizeof(SuperBlock) + target_snap.snapshot_inode * sizeof(Inode), SEEK_SET);
    read(disk_fd, &snap_inode, sizeof(Inode));

    // Освобождаем блоки данных
    uint32_t blocks_count = (snap_inode.size + sb.block_size - 1) / sb.block_size;
    free_blocks(snap_inode.blocks, blocks_count);

    // Освобождаем inode в битовой карте
    uint32_t inode_byte = target_snap.snapshot_inode / 8;
    uint8_t inode_bit = 1 << (target_snap.snapshot_inode % 8);
    inode_bitmap[inode_byte] &= ~inode_bit;
    sb.free_inodes++;

    // 2. Обновляем оригинальный файл
    Inode orig_inode;
    lseek(disk_fd, sizeof(SuperBlock) + target_snap.original_inode * sizeof(Inode), SEEK_SET);
    read(disk_fd, &orig_inode, sizeof(Inode));
    orig_inode.snapshot_count--;
    lseek(disk_fd, sizeof(SuperBlock) + target_snap.original_inode * sizeof(Inode), SEEK_SET);
    write(disk_fd, &orig_inode, sizeof(Inode));

    // 3. Удаляем из массива снапшотов
    memmove(&snapshots[found_index],
           &snapshots[found_index + 1],
           (sb.snapshot_count - found_index - 1) * sizeof(Snapshot));
    sb.snapshot_count--;

    // 4. Сохраняем изменения
    save_metadata();
    close(disk_fd);

    printf("Snapshot '%s' deleted successfully\n", snap_name);
}
void create_file(const char* filename, const void* data) {
    disk_fd = open(DEVICE_PATH, O_RDWR);
    load_metadata();
    // Поиск свободного inode (начиная с 1)
    uint32_t inode_num = find_free_inode();
    if(inode_num == (uint32_t)-1) {
        printf("No free inodes!\n");
        close(disk_fd);
        return;
    }
    // Выделение блоков
    size_t size = strlen(data);
    uint32_t blocks_needed = (size + sb.block_size - 1) / sb.block_size;
    uint32_t blocks[12] = {0};

    for(int i = 0; i < blocks_needed; i++) {
        blocks[i] = allocate_block();
        if(!blocks[i]) {
            printf("No space!\n");
            free_blocks(blocks, i);
            close(disk_fd);
            return;
        }
        lseek(disk_fd, blocks[i] * sb.block_size, SEEK_SET);
        ssize_t bytes_written = write(disk_fd, (char*)data + i*sb.block_size, sb.block_size);
        if (bytes_written != sb.block_size) {
	   perror("[ERROR] Write failed");
	   return;
    	}
    }
    // Создание inode
    Inode node = {
        .used = 1,
        .type = 0,
        .size = size,
        .created = time(0),
        .modified = time(0)

    };
    strncpy(node.name, filename, MAX_NAME_LEN-1);
    memcpy(node.blocks, blocks, sizeof(blocks));
    lseek(disk_fd, sizeof(SuperBlock) + inode_num*sizeof(Inode), SEEK_SET);
    write(disk_fd, &node, sizeof(Inode));
    // Обновление битмапов
    inode_bitmap[inode_num/8] |= 1 << (inode_num%8);
    sb.free_inodes--;
    sb.free_blocks -= blocks_needed;
    save_metadata();
    close(disk_fd);
    printf("Created file '%s' in inode %u\n", filename, inode_num);
}
/*
void create_snapshot(const char* filename, const char* snap_name) {
    disk_fd = open(DEVICE_PATH, O_RDWR);
    load_metadata();
    // Поиск файла
    uint32_t inode_num = find_inode(filename);
    if(inode_num == (uint32_t)-1) {
        printf("File not found!\n");
        close(disk_fd);
        return;
    }
    // Чтение оригинального inode
    Inode original;
    lseek(disk_fd, sizeof(SuperBlock) + inode_num*sizeof(Inode), SEEK_SET);
    read(disk_fd, &original, sizeof(Inode));
    // Создание снапшота
    Snapshot snap = {
        .snap_id = sb.next_snap_id++,
        .timestamp = time(0),
        .original_inode = inode_num,
        .inode = original
    };
    strncpy(snap.snapshot_name, snap_name, MAX_NAME_LEN-1);
    // Копирование данных
    uint32_t blocks_needed = (original.size + sb.block_size - 1) / sb.block_size;
    snap.data = malloc(blocks_needed * sb.block_size);

    for(int i = 0; i < blocks_needed; i++) {
        lseek(disk_fd, original.blocks[i] * sb.block_size, SEEK_SET);
        read(disk_fd, snap.data + i*sb.block_size, sb.block_size);
    }
    // Сохранение снапшота
    snapshots[sb.snapshot_count++] = snap;
    save_metadata();
    close(disk_fd);
    printf("Created snapshot '%s' (ID: %u)\n", snap_name, snap.snap_id);
}*/
void create_snapshot(const char* filename, const char* snap_name) {
    disk_fd = open(DEVICE_PATH, O_RDWR);
    load_metadata();

    // Находим исходный inode
    uint32_t orig_inode = find_inode(filename);
    if(orig_inode == (uint32_t)-1) {
        printf("File not found!\n");
        close(disk_fd);
        return;
    }

    // Создаем новый inode для снапшота
    uint32_t snap_inode = find_free_inode();
    if(snap_inode == (uint32_t)-1) {
        printf("No free inodes!\n");
        close(disk_fd);
        return;
    }

    // Копируем данные исходного inode
    Inode orig_node, snap_node;
    lseek(disk_fd, sizeof(SuperBlock) + orig_inode*sizeof(Inode), SEEK_SET);
    read(disk_fd, &orig_node, sizeof(Inode));

    // Копируем метаданные
    memcpy(&snap_node, &orig_node, sizeof(Inode));
    snap_node.snapshot_count = 0;
    snap_node.modified = time(0);

    snap_node.is_snapshot = 1;
    snap_node.snapshot_parent = orig_inode;

    // Копируем данные в новые блоки
    uint32_t blocks_needed = (orig_node.size + sb.block_size - 1)/sb.block_size;
    for(int i=0; i<blocks_needed; i++) {
        uint32_t new_block = allocate_block();
        if(!new_block) {
            printf("No space for snapshot!\n");
            free_blocks(snap_node.blocks, i);
            close(disk_fd);
            return;
        }

        // Копируем данные блока
        uint8_t* buffer = malloc(sb.block_size);
        lseek(disk_fd, orig_node.blocks[i] * sb.block_size, SEEK_SET);
        read(disk_fd, buffer, sb.block_size);

        lseek(disk_fd, new_block * sb.block_size, SEEK_SET);
        write(disk_fd, buffer, sb.block_size);
        free(buffer);

        snap_node.blocks[i] = new_block;
    }

    // Сохраняем новый inode
    lseek(disk_fd, sizeof(SuperBlock) + snap_inode*sizeof(Inode), SEEK_SET);
    write(disk_fd, &snap_node, sizeof(Inode));

    // Обновляем оригинальный inode
    orig_node.snapshot_count++;
    lseek(disk_fd, sizeof(SuperBlock) + orig_inode*sizeof(Inode), SEEK_SET);
    write(disk_fd, &orig_node, sizeof(Inode));

    // Создаем запись снапшота
    Snapshot snap = {
        .original_inode = orig_inode,
        .snapshot_inode = snap_inode,
        .timestamp = time(0)
    };
    strncpy(snap.snapshot_name, snap_name, MAX_NAME_LEN-1);

    snapshots[sb.snapshot_count++] = snap;
    save_metadata();

    close(disk_fd);
    printf("Snapshot '%s' created (inode %u)\n", snap_name, snap_inode);
}

/*void restore_snapshot(const char* filename, const char* snap_name) {
    disk_fd = open(DEVICE_PATH, O_RDWR);
    load_metadata();

    // Поиск файла
    uint32_t inode_num = find_inode(filename);
    if (inode_num == (uint32_t)-1) {
        printf("File '%s' not found\n", filename);
        close(disk_fd);
        return;
    }

    // Поиск снапшота
    Snapshot *target_snap = NULL;
    for (int i = 0; i < sb.snapshot_count; i++) {
        if (strcmp(snapshots[i].snapshot_name, snap_name) == 0) {
            target_snap = &snapshots[i];
            break;
        }
    }

    if (!target_snap) {
        printf("Snapshot '%s' not found\n", snap_name);
        close(disk_fd);
        return;
    }

    // Полная замена inode из снапшота
    Inode restored_inode = target_snap->inode;

    // Сохраняем оригинальное имя (если не должно меняться)
    strncpy(restored_inode.name, filename, MAX_NAME_LEN-1);

    // Обновление временных меток
    restored_inode.modified = time(NULL);

    // Освобождаем старые блоки
    //Inode current_inode;

    Inode curr_node;
	lseek(disk_fd, sizeof(SuperBlock) + inode_num*sizeof(Inode), SEEK_SET);
	read(disk_fd, &curr_node, sizeof(Inode));

	curr_node.blocks = target->snapshot_inode; // Просто меняем ссылку
	lseek(disk_fd, sizeof(SuperBlock) + inode_num*sizeof(Inode), SEEK_SET);
	write(disk_fd, &curr_node, sizeof(Inode));

    lseek(disk_fd, sizeof(SuperBlock) + inode_num * sizeof(Inode), SEEK_SET);
    read(disk_fd, &current_inode, sizeof(Inode));
    free_blocks(current_inode.blocks,
               (current_inode.size + sb.block_size - 1) / sb.block_size);

    // Выделяем новые блоки
    uint32_t blocks_needed = (restored_inode.size + sb.block_size - 1) / sb.block_size;
    for (int i = 0; i < blocks_needed; i++) {
        restored_inode.blocks[i] = allocate_block();
        if (!restored_inode.blocks[i]) {
            printf("No space to restore\n");
            free_blocks(restored_inode.blocks, i);
            close(disk_fd);
            return;
        }

        // Записываем данные снапшота
        lseek(disk_fd, restored_inode.blocks[i] * sb.block_size, SEEK_SET);
        write(disk_fd,
             target_snap->data + i * sb.block_size,
             sb.block_size);
    }

    // Записываем обновленный inode
    lseek(disk_fd, sizeof(SuperBlock) + inode_num * sizeof(Inode), SEEK_SET);
    write(disk_fd, &restored_inode, sizeof(Inode));
*/
    // Обновление битмапов
  /*  save_metadata();
    close(disk_fd);
    printf("Snapshot '%s' fully restored to '%s'\n", snap_name, filename);
}*/
void restore_snapshot(const char* filename, const char* snap_name) {
    disk_fd = open(DEVICE_PATH, O_RDWR);
    load_metadata();

    // Находим текущий inode файла
    uint32_t curr_inode = find_inode(filename);
    if(curr_inode == (uint32_t)-1) {
        printf("File not found!\n");
        close(disk_fd);
        return;
    }

    // Находим снапшот
    Snapshot *target = NULL;
    for(int i=0; i<sb.snapshot_count; i++) {
        if(strcmp(snapshots[i].snapshot_name, snap_name) == 0) {
            target = &snapshots[i];
            break;
        }
    }

    if(!target) {
        printf("Snapshot not found!\n");
        close(disk_fd);
        return;
    }

    // Читаем данные снапшота
    Inode snap_node;
    lseek(disk_fd, sizeof(SuperBlock) + target->snapshot_inode*sizeof(Inode), SEEK_SET);
    read(disk_fd, &snap_node, sizeof(Inode));

    // Освобождаем старые блоки файла
    Inode curr_node;
    lseek(disk_fd, sizeof(SuperBlock) + curr_inode*sizeof(Inode), SEEK_SET);
    read(disk_fd, &curr_node, sizeof(Inode));
    free_blocks(curr_node.blocks, (curr_node.size + sb.block_size - 1)/sb.block_size);

    // Копируем данные из снапшота
    curr_node.size = snap_node.size;
    curr_node.modified = time(0);
    memcpy(curr_node.blocks, snap_node.blocks, sizeof(curr_node.blocks));

    // Записываем обновленный inode
    lseek(disk_fd, sizeof(SuperBlock) + curr_inode*sizeof(Inode), SEEK_SET);
    write(disk_fd, &curr_node, sizeof(Inode));

    save_metadata();
    close(disk_fd);
    printf("Restored snapshot '%s' for file '%s'\n", snap_name, filename);
}
/*void restore_snapshot(const char* filename, const char* snap_name) {
    disk_fd = open(DEVICE_PATH, O_RDWR);
    load_metadata();
    // Поиск файла
    uint32_t inode_num = find_inode(filename);
    if(inode_num == (uint32_t)-1) {
        printf("File '%s' not found\n", filename);
        close(disk_fd);
        return;
    }
    // Поиск снапшота по имени
    Snapshot *target_snap = NULL;
    for(int i = 0; i < sb.snapshot_count; i++) {
        if(strcmp(snapshots[i].snapshot_name, snap_name) == 0) {
            target_snap = &snapshots[i];
            break;
        }
    }

    if(!target_snap) {
        printf("Snapshot '%s' not found\n", snap_name);
        close(disk_fd);
        return;
    }
    // Проверка принадлежности снапшота файлу
    if(target_snap->original_inode != inode_num) {
        printf("Snapshot '%s' doesn't belong to '%s'\n", snap_name, filename);
        close(disk_fd);
        return;
    }
    // Чтение текущего inode
    Inode current;
    lseek(disk_fd, sizeof(SuperBlock) + inode_num * sizeof(Inode), SEEK_SET);
    read(disk_fd, &current, sizeof(Inode));
    // Освобождение старых блоков файла
    uint32_t old_blocks = (current.size + sb.block_size - 1) / sb.block_size;
    //free_blocks(current.blocks, old_blocks);
    // Копирование данных из снапшота*/
    /*uint32_t new_blocks = (target_snap->inode.size + sb.block_size - 1) / sb.block_size;
    for(int i = 0; i < new_blocks; i++) {
        uint32_t new_block = allocate_block();
        if(!new_block) {
            printf("No space to restore\n");
            free_blocks(target_snap->inode.blocks, i);
            close(disk_fd);
            return;
        }
        // Запись данных снапшота в новые блоки
        lseek(disk_fd, new_block * sb.block_size, SEEK_SET);
        write(disk_fd, target_snap->data + i * sb.block_size, sb.block_size);

        current.blocks[i] = new_block;
    }*/
    // Обновление метаданных файла
    /*current.size = target_snap->inode.size;
    //current.modified = time(NULL);
    current.snapshot_id = 0; // Сбрасываем привязку
    lseek(disk_fd, sizeof(SuperBlock) + inode_num * sizeof(Inode), SEEK_SET);
    write(disk_fd, &current, sizeof(Inode));
    save_metadata();
    close(disk_fd);
    printf("Successfully restored snapshot '%s' to '%s'\n", snap_name, filename);
}*/
void free_blocks(uint32_t* blocks, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (blocks[i] == 0) continue;
        uint32_t byte = blocks[i] / 8;
        uint8_t bit = 1 << (blocks[i] % 8);
        if (block_bitmap[byte] & bit) {
            block_bitmap[byte] &= ~bit;
            sb.free_blocks++;
        }
        blocks[i] = 0; // Важно обнулить!
    }
}
uint32_t allocate_block() {
    for (uint32_t i = sb.first_data_block; i < sb.total_blocks; i++) {
        uint32_t byte = i / 8;
        uint8_t bit = 1 << (i % 8);
        if (!(block_bitmap[byte] & bit)) {
            block_bitmap[byte] |= bit;
            sb.free_blocks--;
            return i;
        }
    }
    printf("[ERROR] No free blocks available!\n");
    return 0; // Невалидный блок
}
uint32_t find_inode(const char* filename) {
    for (uint32_t i = 0; i < sb.inode_count; i++) {
        if (!(inode_bitmap[i/8] & (1 << (i%8)))) continue;

        Inode node;
        lseek(disk_fd, sizeof(SuperBlock) + i * sizeof(Inode), SEEK_SET);
        read(disk_fd, &node, sizeof(Inode));

        if (node.used && strcmp(node.name, filename) == 0) {
            return i;
        }
    }
    return (uint32_t)-1;
}
void save_metadata() {
    lseek(disk_fd, 0, SEEK_SET);
    if(0 > write(disk_fd, &sb, sizeof(SuperBlock)))
    {
    	printf("Error save meta1\n");
    	return;
    }
    if(0 > write(disk_fd, block_bitmap, sb.total_blocks / 8))
    {
    	printf("Error save meta2\n");
    	return;
    }
    if(0 > write(disk_fd, inode_bitmap, sb.inode_count / 8))
    {
    	printf("Error save meta3\n");
    	return;
   }
	// Сохранение снапшотов в выделенные блоки
	   uint32_t snap_blocks = (sizeof(Snapshot) * MAX_SNAPSHOTS + sb.block_size - 1) / sb.block_size;
	   uint32_t start_block = sb.first_data_block + 10; // Резервируем 10 блоков после first_data_block

	   lseek(disk_fd, start_block * sb.block_size, SEEK_SET);
	   if (write(disk_fd, snapshots, sizeof(Snapshot) * MAX_SNAPSHOTS) == -1) {
		   perror("Error saving snapshots");
		   return;
	   }


    if (DEBUG) {
        printf("[DEBUG] Saved metadata:\n");
        printf("  Free inodes: %u\n", sb.free_inodes);
        printf("  Free blocks: %u\n", sb.free_blocks);
    }
}
void load_metadata() {
    lseek(disk_fd, 0, SEEK_SET);
    read(disk_fd, &sb, sizeof(SuperBlock));

    //free(block_bitmap);
    //free(inode_bitmap);

    block_bitmap = malloc(sb.total_blocks/8);
    inode_bitmap = malloc(sb.inode_count/8);

    read(disk_fd, block_bitmap, sb.total_blocks/8);
    read(disk_fd, inode_bitmap, sb.inode_count/8);

    // Загрузка снапшотов из специальных блоков
    uint32_t snap_blocks = (sizeof(Snapshot) * MAX_SNAPSHOTS) / sb.block_size + 1;
    uint32_t start_block = sb.first_data_block + 10;

    lseek(disk_fd, start_block * sb.block_size, SEEK_SET);
    read(disk_fd, snapshots, sizeof(Snapshot) * MAX_SNAPSHOTS);
}
void print_file_content(const char* filename) {
    disk_fd = open(DEVICE_PATH, O_RDONLY);
    load_metadata();
    uint32_t inode_num = find_inode(filename);
    if(inode_num == (uint32_t)-1) {
        printf("File not found\n");
        close(disk_fd);
        return;
    }
    Inode node;
    lseek(disk_fd, sizeof(SuperBlock) + inode_num * sizeof(Inode), SEEK_SET);
    read(disk_fd, &node, sizeof(Inode));
    /*if(node.size == 0) {
	printf("<EMPTY FILE>\n");
	close(disk_fd);
        return;
    }
*/
    printf("\nContents of '%s' (%u bytes):\n", filename, node.size);
    printf("--------------------------------------------------\n");
    uint32_t blocks_needed = (node.size + sb.block_size - 1) / sb.block_size;
    char* buffer = malloc(sb.block_size);
    for(int i = 0; i < blocks_needed; i++) {
        lseek(disk_fd, node.blocks[i] * sb.block_size, SEEK_SET);
        read(disk_fd, buffer, sb.block_size);

        size_t bytes_to_print = (i == blocks_needed-1)
                              ? node.size % sb.block_size
                              : sb.block_size;
        if(bytes_to_print == 0) bytes_to_print = sb.block_size;

        fwrite(buffer, 1, bytes_to_print, stdout);
    }
    free(buffer);
    printf("\n--------------------------------------------------\n");
    close(disk_fd);
}
void list_snapshots() {
    disk_fd = open(DEVICE_PATH, O_RDONLY);
    load_metadata();
    printf("\n%-20s %-20s %-30s %-10s %s\n",
           "Snapshot Name", "File", "Timestamp", "Size", "Inode");
    printf("----------------------------------------------------------------------------------------\n");

    if (sb.snapshot_count == 0) {
        printf("No snapshots available\n");
        return;
    }
    for (int i = 0; i < sb.snapshot_count; i++) {
        char time_buf[30];
        struct tm *tm_info = localtime(&snapshots[i].timestamp);
        strftime(time_buf, 30, "%Y-%m-%d %H:%M:%S", tm_info);

        printf("%-20s %-20s %-30s %-10u %u\n",
               snapshots[i].snapshot_name,
               snapshots[i].inode.name,
               time_buf,
               snapshots[i].inode.size,
               snapshots[i].original_inode);
    }
    close(disk_fd);
}
int main(int argc, char *argv[]) {
    int opt;
    int zero_fill = 0;
    uint32_t block_size = 4096;
    char *filename = NULL, *data = NULL, *snap_name = NULL;
    while ((opt = getopt(argc, argv, "0b:f:lc:s:r:e:d:phq:wx:")) != -1) {
        switch (opt) {
            case 'b': block_size = atoi(optarg); break;
            case 'f': {
            	case '0': {format_disk(0, block_size); return 0;}
            	case '1': {format_disk(1, block_size); return 0;}
            }
            case 'l': list_files(); return 0;
            case 'w': list_snapshots(); return 0;
            case 'c': filename = optarg; data = argv[optind++];
                     create_file(filename, data); return 0;
            case 's': filename = optarg; snap_name = argv[optind++];
                     create_snapshot(filename, snap_name); return 0;
            case 'r': filename = optarg; snap_name = argv[optind++];
                     restore_snapshot(filename, snap_name); return 0;
            case 'e': filename = optarg; data = argv[optind++];
                     edit_file(filename, data); return 0;
            case 'd': delete_file(optarg); return 0;
            case 'p': print_fs_info(); return 0;
            case 'q': print_file_content(optarg); return 0;
            case 'x': delete_snapshot(optarg); return 0;
            case 'h':
            default:
                printf("Usage: %s [options]\n"
                       "  -b <size>    Set block size (default 4096)\n"
                       "  -0           Zero fill device on format\n"
                       "  -f           Format device\n"
                       "  -c <f> <d>   Create file\n"
                       "  -l           List files\n"
                       "  -w           List snapshots\n"
                       "  -q <f>       Cat file\n"
                       "  -s <f> <n>   Create snapshot\n"
                       "  -r <f> <n>   Restore snapshot\n"
                       "  -e <f> <d>   Edit file\n"
                       "  -d <f>       Delete file\n"
                	   "  -x <f>       Delete snapshot\n"
                       "  -p           Print FS info\n",
                       argv[0]);
                return 0;
        }
    }
    return 0;
}
