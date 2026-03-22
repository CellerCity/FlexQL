#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "pager.h"

// 1. Open the file and initialize the Pager
Pager* pager_open(const char* filename) {
    // Open file for Read/Write. Create it if it doesn't exist.
    // S_IRUSR | S_IWUSR gives the owner read/write permissions.
    int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    
    if (fd == -1) {
        perror("[-] Unable to open database file");
        exit(EXIT_FAILURE);
    }

    // Find the total length of the file using lseek
    off_t file_length = lseek(fd, 0, SEEK_END);

    Pager* pager = malloc(sizeof(Pager));
    pager->file_descriptor = fd;
    pager->file_length = file_length;
    
    // Calculate how many 4KB pages currently exist in the file
    pager->num_pages = (file_length / PAGE_SIZE);

    // Initialize the cache array to NULL
    if (file_length % PAGE_SIZE != 0) {
        printf("[-] Warning: Database file size is not a multiple of PAGE_SIZE.\n");
    }

    for (uint32_t i = 0; i < MAX_PAGES; i++) {
        pager->pages[i] = NULL;
    }

    return pager;
}

// 2. Fetch a page from RAM, or load it from disk
Page* get_page(Pager* pager, uint32_t page_num) {
    if (page_num >= MAX_PAGES) {
        printf("[-] Error: Page number out of bounds. Max is %d\n", MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    // Cache Hit: The page is already in RAM! Return it instantly.
    if (pager->pages[page_num] != NULL) {
        return pager->pages[page_num];
    }

    // Cache Miss: We need to allocate memory and load it from disk
    Page* page = malloc(sizeof(Page));

    // Calculate exactly where this page starts in the .dat file
    uint32_t offset = page_num * PAGE_SIZE;

    // If the requested page already exists on disk, read it
    if (page_num < pager->num_pages) {
        lseek(pager->file_descriptor, offset, SEEK_SET);
        ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
        
        if (bytes_read == -1) {
            perror("[-] Error reading file");
            exit(EXIT_FAILURE);
        }
    } else {
        // This is a brand NEW page. Initialize its header.
        page->header.page_id = page_num;
        page->header.num_slots = 0;
        // Free space starts at the very end of the 4KB block
        page->header.free_space_ptr = PAGE_SIZE; 
    }

    // Save it in our cache array for next time
    pager->pages[page_num] = page;

    return page;
}

// 3. Save a modified page back to the disk
void pager_flush(Pager* pager, uint32_t page_num) {
    if (pager->pages[page_num] == NULL) {
        printf("[-] Error: Tried to flush a null page.\n");
        exit(EXIT_FAILURE);
    }

    uint32_t offset = page_num * PAGE_SIZE;
    lseek(pager->file_descriptor, offset, SEEK_SET);
    
    ssize_t bytes_written = write(pager->file_descriptor, pager->pages[page_num], PAGE_SIZE);

    if (bytes_written == -1) {
        perror("[-] Error writing to disk");
        exit(EXIT_FAILURE);
    }
}

// 4. Safely close everything
void pager_close(Pager* pager) {
    // Flush all loaded pages to disk before closing
    for (uint32_t i = 0; i < MAX_PAGES; i++) {
        if (pager->pages[i] != NULL) {
            pager_flush(pager, i);
            free(pager->pages[i]);
            pager->pages[i] = NULL;
        }
    }

    close(pager->file_descriptor);
    free(pager);
}