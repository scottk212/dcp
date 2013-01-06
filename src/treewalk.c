/*
 * This file contains the logic to walk all of the source objects and place
 * them on the queue.
 *
 * In the case of directories, we'll simply read the contents of a directory
 * and place each sub-object back on the queue to be treewalked again. In the
 * case of files, we chunk up each file and place it on the queue as another
 * libcircle action to be later processed by the COPY and CLEANUP stages.
 *
 * See the file "COPYING" for the full license governing this code.
 */

#include "treewalk.h"
#include "dcp.h"

#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>

/** Options specified by the user. */
extern DCOPY_options_t DCOPY_user_opts;

/**
 * Determine if the specified path is a directory.
 */
bool DCOPY_is_directory(char* path)
{
    struct stat statbuf;

    if(lstat(path, &statbuf) < 0) {
        LOG(DCOPY_LOG_ERR, "Could not get info for `%s'. %s", path, strerror(errno));
        return false;
    }

    return (S_ISDIR(statbuf.st_mode) && !(S_ISLNK(statbuf.st_mode)));
}

/**
 * Determine if the specified path is a regular file.
 */
bool DCOPY_is_regular_file(char* path)
{
    struct stat statbuf;

    if(lstat(path, &statbuf) < 0) {
        LOG(DCOPY_LOG_ERR, "Could not get info for `%s'. %s", path, strerror(errno));
        return false;
    }

    return (S_ISREG(statbuf.st_mode) && !(S_ISLNK(statbuf.st_mode)));
}

/**
 * This is the entry point for the "file stat stage". This function is called
 * from the jump table required for the main libcircle callbacks.
 */
void DCOPY_do_treewalk(DCOPY_operation_t* op, CIRCLE_handle* handle)
{
    struct stat statbuf;
    char* newop;

    if(lstat(op->operand, &statbuf) < 0) {
        if(DCOPY_user_opts.reliable_filesystem) {
            LOG(DCOPY_LOG_DBG, "Could not get info for `%s'. %s", op->operand, strerror(errno));
            exit(EXIT_FAILURE);
        }
        else {
            /* Retry the treewalk operation. */
            newop = DCOPY_encode_operation(TREEWALK, op->chunk, op->operand, \
                                           op->source_base_offset, op->dest_base_appendix, op->file_size);
            handle->enqueue(newop);
            free(newop);

            return;
        }
    }

    if(S_ISDIR(statbuf.st_mode) && !(S_ISLNK(statbuf.st_mode))) {
        /* LOG(DCOPY_LOG_DBG, "Stat operation found a directory at `%s'.", op->operand); */
        DCOPY_stat_process_dir(op, handle);
    }
    else if(S_ISREG(statbuf.st_mode) && !(S_ISLNK(statbuf.st_mode))) {
        /* LOG(DCOPY_LOG_DBG, "Stat operation found a file at `%s'.", op->operand); */
        DCOPY_stat_process_file(op, (uint64_t)statbuf.st_size, handle);
    }
    else {
        LOG(DCOPY_LOG_DBG, "Encountered an unsupported file type at `%s'.", op->operand);

        if(DCOPY_user_opts.reliable_filesystem) {
            exit(EXIT_FAILURE);
        }
        else {
            LOG(DCOPY_LOG_DBG, "Since unreliable filesystem was specified, we're attempting to look at the file again.");

            /* Retry the treewalk operation. */
            newop = DCOPY_encode_operation(TREEWALK, op->chunk, op->operand, \
                                           op->source_base_offset, op->dest_base_appendix, op->file_size);
            handle->enqueue(newop);
            free(newop);

            return;
        }
    }
}

/**
 * This function inputs a file and creates chunk operations that get placed
 * onto the libcircle queue for future processing by the copy stage.
 */
void DCOPY_stat_process_file(DCOPY_operation_t* op, uint64_t file_size, CIRCLE_handle* handle)
{
    uint32_t chunk_index;
    uint32_t num_chunks = (uint32_t)file_size / DCOPY_CHUNK_SIZE;

    LOG(DCOPY_LOG_DBG, "File `%s' size is `%" PRIu64 "' with chunks `%" PRIu32 "' (total `%" PRIu32 "').", \
        op->operand, file_size, num_chunks, num_chunks * DCOPY_CHUNK_SIZE);

    /* Encode and enqueue each chunk of the file for processing later. */
    for(chunk_index = 0; chunk_index < num_chunks; chunk_index++) {
        char* newop = DCOPY_encode_operation(COPY, chunk_index, op->operand, op->source_base_offset, op->dest_base_appendix, file_size);
        handle->enqueue(newop);
        free(newop);
    }

    /* Encode and enqueue the last partial chunk. */
    if(num_chunks * DCOPY_CHUNK_SIZE < file_size) {
        char* newop = DCOPY_encode_operation(COPY, chunk_index, op->operand, op->source_base_offset, op->dest_base_appendix, file_size);
        handle->enqueue(newop);
        free(newop);
    }
}

/**
 * This function reads the contents of a directory and generates appropriate
 * libcircle operations for every object in the directory. It then places those
 * operations on the libcircle queue and returns.
 */
void DCOPY_stat_process_dir(DCOPY_operation_t* op, CIRCLE_handle* handle)
{
    DIR* curr_dir;
    char* curr_dir_name;
    char* newop;

    struct dirent* curr_ent;
    char cmd_buf[PATH_MAX];
    char newop_path[PATH_MAX];

    if(op->dest_base_appendix != NULL) {
        sprintf(cmd_buf, "mkdir -p %s/%s/%s", \
                DCOPY_user_opts.dest_path, \
                op->dest_base_appendix, \
                op->operand + op->source_base_offset);
    }
    else {
        sprintf(cmd_buf, "mkdir -p %s/%s", \
                DCOPY_user_opts.dest_path, \
                op->operand + op->source_base_offset);
    }

    LOG(DCOPY_LOG_DBG, "Creating directory with command `%s'.", cmd_buf);

    FILE* p = popen(cmd_buf, "r");
    pclose(p);

    curr_dir = opendir(op->operand);

    if(curr_dir == NULL) {
        LOG(DCOPY_LOG_ERR, "Unable to open dir `%s'. %s", \
            op->operand, strerror(errno));

        if(DCOPY_user_opts.reliable_filesystem) {
            exit(EXIT_FAILURE);
        }
        else {
            LOG(DCOPY_LOG_DBG, "Since unreliable filesystem was specified, we're attempting to look at the directory again.");

            /* Retry the entire stat operation. */
            newop = DCOPY_encode_operation(TREEWALK, op->chunk, op->operand, \
                                           op->source_base_offset, op->dest_base_appendix, op->file_size);
            handle->enqueue(newop);
            free(newop);

            return;
        }
    }
    else {
        while((curr_ent = readdir(curr_dir)) != NULL) {
            curr_dir_name = curr_ent->d_name;

            /* We don't care about . or .. */
            if((strncmp(curr_dir_name, ".", 2)) && (strncmp(curr_dir_name, "..", 3))) {

                LOG(DCOPY_LOG_DBG, "Stat operation is enqueueing directory `%s' using base `%s'.", \
                    op->operand, op->operand + op->source_base_offset);

                sprintf(newop_path, "%s/%s", op->operand, curr_dir_name);

                newop = DCOPY_encode_operation(TREEWALK, 0, newop_path, \
                                               op->source_base_offset, op->dest_base_appendix, op->file_size);
                handle->enqueue(newop);

                free(newop);
            }
        }
    }

    closedir(curr_dir);
    return;
}

/* EOF */