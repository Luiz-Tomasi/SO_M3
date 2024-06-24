#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "commands.h"
#include "fat16.h"
#include "support.h"

off_t fsize(const char *filename){
    struct stat st;
    if (stat(filename, &st) == 0)
        return st.st_size;
    return -1;
}

struct fat_dir find(struct fat_dir *dirs, char *filename, struct fat_bpb *bpb){
    struct fat_dir curdir;
    int dirs_len = sizeof(struct fat_dir) * bpb->possible_rentries;
    int i;

    for (i=0; i < dirs_len; i++){
        if (strcmp((char *) dirs[i].name, filename) == 0){
            curdir = dirs[i];
            break;
        }
    }
    return curdir;
}

struct fat_dir *ls(FILE *fp, struct fat_bpb *bpb){
    int i;
    struct fat_dir *dirs = malloc(sizeof (struct fat_dir) * bpb->possible_rentries);

    for (i=0; i < bpb->possible_rentries; i++){
        uint32_t offset = bpb_froot_addr(bpb) + i * 32;
        read_bytes(fp, offset, &dirs[i], sizeof(dirs[i]));
    }
    return dirs;
}

struct fat_dir *find_dir(FILE *fp, char *filename, struct fat_bpb *bpb){
    unsigned int dir_offset = bpb->reserved_sect * bpb->bytes_p_sect + bpb->sect_per_fat * bpb->bytes_p_sect;
    unsigned int dir_entries_per_sector = bpb->bytes_p_sect / sizeof(struct fat_dir);
    unsigned int dir_entries_per_fat = dir_entries_per_sector * bpb->sect_per_fat;
    unsigned int dir_entries_per_cluster = dir_entries_per_sector * bpb->sector_p_clust;
    unsigned int dir_entries_per_file = dir_entries_per_cluster * bpb->clust_p_fat;
    unsigned int dir_entries_per_file_system = dir_entries_per_file * bpb->clust_p_fat;

    unsigned int dir_entries_per_sector_in_bytes = sizeof(struct fat_dir) * dir_entries_per_sector;

    unsigned int dir_entry_pos = dir_offset;
    unsigned int dir_entry_count = 0;

    struct fat_dir *dir = NULL;

    while (dir_entry_count < dir_entries_per_file_system){
        fseek(fp, dir_entry_pos, SEEK_SET);
        fread(dir, sizeof(struct fat_dir), dir_entries_per_sector, fp);

        if (strcmp(dir->name, filename) == 0){
            return dir;
        }

        dir_entry_count += dir_entries_per_sector;
        dir_entry_pos += dir_entries_per_sector_in_bytes;
    }

    return NULL;
}

int write_dir(FILE *fp, char *fname, struct fat_dir *dir){
    char* name = padding(fname);
    strcpy((char *) dir->name, (char *) name);
    if (fwrite(dir, 1, sizeof(struct fat_dir), fp) <= 0)
        return -1;
    return 0;
}

int remove_dir(FILE *fp, struct fat_dir *dir, struct fat_bpb *bpb, char *filename){
    unsigned int dir_offset = bpb->reserved_sect * bpb->bytes_p_sect + bpb->sect_per_fat * bpb->bytes_p_sect;
    unsigned int dir_entries_per_sector = bpb->bytes_p_sect / sizeof(struct fat_dir);
    unsigned int dir_entries_per_fat = dir_entries_per_sector * bpb->sect_per_fat;
    unsigned int dir_entries_per_cluster = dir_entries_per_sector * bpb->sector_p_clust;
    unsigned int dir_entries_per_file = dir_entries_per_cluster * bpb->clust_p_fat;
    unsigned int dir_entries_per_file_system = dir_entries_per_file * bpb->clust_p_fat;

    unsigned int dir_entries_per_sector_in_bytes = sizeof(struct fat_dir) * dir_entries_per_sector;

    unsigned int dir_entry_pos = dir_offset;
    unsigned int dir_entry_count = 0;

    while (dir_entry_count < dir_entries_per_file_system){
        fseek(fp, dir_entry_pos, SEEK_SET);
        fread(dir, sizeof(struct fat_dir), dir_entries_per_sector, fp);

        if (strcmp(dir->name, filename) == 0){
            memset(dir, 0, sizeof(struct fat_dir));
            fseek(fp, dir_entry_pos, SEEK_SET);
            fwrite(dir, sizeof(struct fat_dir), dir_entries_per_sector, fp);
            return 0;
        }

        dir_entry_count += dir_entries_per_sector;
        dir_entry_pos += dir_entries_per_sector_in_bytes;
    }

    return -1;
}

int write_data(FILE *fp, char *fname, struct fat_dir *dir, struct fat_bpb *bpb){

    FILE *localf = fopen(fname, "r");
    int c;

    while ((c = fgetc(localf)) != EOF){
        if (fputc(c, fp) != c)
            return -1;
    }
    return 0;
}

int wipe(FILE *fp, struct fat_dir *dir, struct fat_bpb *bpb){
    int start_offset = bpb_froot_addr(bpb) + (bpb->bytes_p_sect * \
            dir->starting_cluster);
    int limit_offset = start_offset + dir->file_size;

    while (start_offset <= limit_offset){
        fseek(fp, ++start_offset, SEEK_SET);
        if(fputc(0x0, fp) != 0x0)
            return 01;
    }
    return 0;
}

int rename_dir(FILE *fp, struct fat_dir *dir, char *new_filename, struct fat_bpb *bpb){
    // Update the name of the file in the directory entry
    strncpy(dir->name, new_filename, 11);
    dir->name[11] = '\0';

    // Update the starting cluster of the file in the directory entry
    dir->starting_cluster = next_free_cluster(fp, bpb);

    // Write the updated directory entry back to the FAT
    write_dir_entry(fp, dir, bpb);

    return 0;
}

void mv(FILE *fp, char *filename, struct fat_bpb *bpb){
    struct fat_dir *dir = find_dir(fp, filename, bpb);
    if (dir == NULL){
        fprintf(stderr, "File not found: %s\n", filename);
        return;
    }

    char *new_filename = get_new_filename();
    if (new_filename == NULL){
        fprintf(stderr, "Error getting new filename\n");
        return;
    }

    struct fat_dir *new_dir = find_dir(fp, new_filename, bpb);
    if (new_dir != NULL){
        fprintf(stderr, "File already exists: %s\n", new_filename);
        free(new_filename);
        return;
    }

    if (rename_dir(fp, dir, new_filename, bpb) != 0){
        fprintf(stderr, "Error renaming file\n");
        free(new_filename);
        return;
    }

    free(new_filename);
}

void rm(FILE *fp, char *filename, struct fat_bpb *bpb){
    struct fat_dir *dir = find_dir(fp, filename, bpb);
    if (dir == NULL){
        fprintf(stderr, "File not found: %s\n", filename);
        return;
    }

    if (wipe(fp, dir, bpb) != 0){
        fprintf(stderr, "Error wiping file\n");
        return;
    }

    if (remove_dir(fp, dir, bpb, filename) != 0){
        fprintf(stderr, "Error removing directory entry\n");
        return;
    }
}

void cp(FILE *fp, char *filename, struct fat_bpb *bpb){
    // Find the file in the FAT directory
    struct fat_dir file = find(ls(fp, bpb), filename, bpb);

    if (file.starting_cluster == 0){
        fprintf(stderr, "File not found: %s\n", filename);
        return;
    }

    // Open the destination file for writing
    FILE *dest_file = fopen(filename, "wb");

    if (dest_file == NULL){
        fprintf(stderr, "Error opening destination file\n");
        return;
    }

    // Read and copy the file contents
    unsigned int cluster = file.starting_cluster;
    unsigned int bytes_remaining = file.file_size;
    unsigned char buffer[bpb->bytes_p_sect];

    while (bytes_remaining > 0){
        unsigned int offset = data_offset(bpb, cluster);
        unsigned int read_size = (bytes_remaining > bpb->bytes_p_sect) ? bpb->bytes_p_sect : bytes_remaining;

        read_bytes(fp, offset, buffer, read_size);
        fwrite(buffer, 1, read_size, dest_file);

        bytes_remaining -= read_size;
        cluster = next_cluster(fp, bpb, cluster);
    }

    fclose(dest_file);
}



