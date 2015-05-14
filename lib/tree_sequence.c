/*
** Copyright (C) 2014 Jerome Kelleher <jerome.kelleher@well.ox.ac.uk>
**
** This file is part of msprime.
**
** msprime is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** msprime is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with msprime.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <hdf5.h>

#include "err.h"
#include "msprime.h"
#include "object_heap.h"

/*
 * Comparator for coalescence records. Sort by left.
 */
static int
cmp_coalescence_record(const void *a, const void *b) {
    const coalescence_record_t *ca = (const coalescence_record_t *) a;
    const coalescence_record_t *cb = (const coalescence_record_t *) b;
    return (ca->left > cb->left) - (ca->left < cb->left);
}

static int
cmp_tree_node_list(const void *a, const void *b) {
    const tree_node_list_t *ia = (const tree_node_list_t *) a;
    const tree_node_list_t *ib = (const tree_node_list_t *) b;
    return (ia->key > ib->key) - (ia->key < ib->key);
}


/* Allocates the memory required for arrays of values. Assumes that
 * the num_breakpoints and num_records has been set.
 */
static int
tree_sequence_alloc(tree_sequence_t *self)
{
    int ret = MSP_ERR_NO_MEMORY;

    self->breakpoints = malloc(self->num_breakpoints * sizeof(uint32_t));
    if (self->breakpoints == NULL) {
        ret = MSP_ERR_NO_MEMORY;
        goto out;
    }
    self->left = malloc(self->num_records * sizeof(uint32_t));
    self->right = malloc(self->num_records * sizeof(uint32_t));
    self->children = malloc(2 * self->num_records * sizeof(uint32_t));
    self->node = malloc(self->num_records * sizeof(uint32_t));
    self->time = malloc(self->num_records * sizeof(double));
    if (self->left == NULL || self->right == NULL || self->children == NULL
            || self->node == NULL || self->time == NULL) {
        goto out;
    }
    ret = 0;
out:
    return ret;
}

int
tree_sequence_free(tree_sequence_t *self)
{
    if (self->breakpoints != NULL) {
        free(self->breakpoints);
    }
    if (self->left != NULL) {
        free(self->left);
    }
    if (self->right != NULL) {
        free(self->right);
    }
    if (self->children != NULL) {
        free(self->children);
    }
    if (self->node != NULL) {
        free(self->node);
    }
    if (self->time != NULL) {
        free(self->time);
    }
    return 0;
}

int
tree_sequence_create(tree_sequence_t *self, msp_t *sim)
{
    int ret = -1;
    uint32_t j;
    coalescence_record_t *records = NULL;

    self->num_breakpoints = msp_get_num_breakpoints(sim);
    self->num_records = msp_get_num_coalescence_records(sim);
    self->sample_size = sim->sample_size;
    self->num_loci = sim->num_loci;
    tree_sequence_alloc(self);
    ret = msp_get_breakpoints(sim, self->breakpoints);
    if (ret != 0) {
        goto out;
    }
    records = malloc(self->num_records * sizeof(coalescence_record_t));
    if (records == NULL) {
        goto out;
    }
    ret = msp_get_coalescence_records(sim, records);
    if (ret != 0) {
        goto out;
    }
    /* Sort the records by the left coordinate*/
    qsort(records, self->num_records, sizeof(coalescence_record_t),
            cmp_coalescence_record);
    for (j = 0; j < self->num_records; j++) {
        self->left[j] = records[j].left;
        self->right[j] = records[j].right;
        self->node[j] = records[j].node;
        self->children[2 * j] = records[j].children[0];
        self->children[2 * j + 1] = records[j].children[1];
        self->time[j] = records[j].time;
    }
    ret = 0;
out:
    if (records != NULL) {
        free(records);
    }
    return ret;
}

/* Reads the metadata for the overall file and updates the basic
 * information in the tree_sequence.
 */
static int
tree_sequence_read_hdf5_metadata(tree_sequence_t *self, hid_t file_id)
{
    int ret = MSP_ERR_HDF5;
    hid_t attr_id, dataspace_id;
    herr_t status;
    int rank;
    hsize_t dims;
    uint32_t version;
    const char *prefixes[] = {"/", "/parameters", "/parameters"};
    const char *names[] = {"format_version", "sample_size", "num_loci"};
    size_t num_attrs = sizeof(prefixes) / sizeof(char *);
    void *destinations[num_attrs];
    unsigned int j;

    destinations[0] = &version;
    destinations[1] = &self->sample_size;
    destinations[2] = &self->num_loci;
    for (j = 0; j < num_attrs; j++) {
        attr_id = H5Aopen_by_name(file_id, prefixes[j], names[j],
                H5P_DEFAULT, H5P_DEFAULT);
        if (attr_id < 0) {
            goto out;
        }
        dataspace_id = H5Aget_space(attr_id);
        if (dataspace_id < 0) {
            goto out;
        }
        rank = H5Sget_simple_extent_ndims(dataspace_id);
        if (rank != 1) {
            ret = MSP_ERR_FILE_FORMAT;
            goto out;
        }
        status = H5Sget_simple_extent_dims(dataspace_id, &dims, NULL);
        if (status < 0) {
            goto out;
        }
        status = H5Aread(attr_id, H5T_NATIVE_UINT32, destinations[j]);
        if (status < 0) {
            goto out;
        }
        status = H5Sclose(dataspace_id);
        if (status < 0) {
            goto out;
        }
        status = H5Aclose(attr_id);
        if (status < 0) {
            goto out;
        }
    }
    ret = 0;
out:
    return ret;
}

static int
tree_sequence_check_hdf5_dimensions(tree_sequence_t *self, hid_t file_id)
{
    int ret = MSP_ERR_HDF5;
    hid_t dataset_id, dataspace_id;
    herr_t status;
    int rank;
    hsize_t dims[2];
    struct _dimension_check {
        const char *name;
        int dimensions;
    };
    struct _dimension_check fields[] = {
        {"/records/left", 1},
        {"/records/right", 1},
        {"/records/node", 1},
        {"/records/children", 2},
        {"/records/time", 1},
    };
    size_t num_fields = sizeof(fields) / sizeof(struct _dimension_check);
    size_t j;

    for (j = 0; j < num_fields; j++) {
        dataset_id = H5Dopen(file_id, fields[j].name, H5P_DEFAULT);
        if (dataset_id < 0) {
            goto out;
        }
        dataspace_id = H5Dget_space(dataset_id);
        if (dataspace_id < 0) {
            goto out;
        }
        rank = H5Sget_simple_extent_ndims(dataspace_id);
        if (rank != fields[j].dimensions) {
            ret = MSP_ERR_FILE_FORMAT;
            goto out;
        }
        status = H5Sget_simple_extent_dims(dataspace_id, dims, NULL);
        if (status < 0) {
            goto out;
        }
        if (dims[0] != self->num_records) {
            ret = MSP_ERR_FILE_FORMAT;
            goto out;
        }
        status = H5Sclose(dataspace_id);
        if (status < 0) {
            goto out;
        }
        status = H5Dclose(dataset_id);
        if (status < 0) {
            goto out;
        }
    }
    ret = 0;
out:
    return ret;
}

/* Reads the dimensions for the breakpoints and records and mallocs
 * space.
 */
static int
tree_sequence_read_hdf5_dimensions(tree_sequence_t *self, hid_t file_id)
{
    int ret = MSP_ERR_HDF5;
    hid_t dataset_id, dataspace_id;
    herr_t status;
    int rank;
    hsize_t dims[2];

    /* breakpoints */
    dataset_id = H5Dopen(file_id, "/breakpoints", H5P_DEFAULT);
    if (dataset_id < 0) {
        goto out;
    }
    dataspace_id = H5Dget_space(dataset_id);
    if (dataspace_id < 0) {
        goto out;
    }
    rank = H5Sget_simple_extent_ndims(dataspace_id);
    if (rank != 1) {
        ret = MSP_ERR_FILE_FORMAT;
        goto out;
    }
    status = H5Sget_simple_extent_dims(dataspace_id, dims, NULL);
    if (status < 0) {
        goto out;
    }
    self->num_breakpoints = (size_t) dims[0];
    self->breakpoints = malloc(self->num_breakpoints * sizeof(uint32_t));
    if (self->breakpoints == NULL) {
        ret = MSP_ERR_NO_MEMORY;
        goto out;
    }
    status = H5Sclose(dataspace_id);
    if (status < 0) {
        goto out;
    }
    status = H5Dclose(dataset_id);
    if (status < 0) {
        goto out;
    }
    /* For the records, we read the dims for left, and base the mallocs
     * on this. We check the others after to make sure they have the
     * same dimensions.
     */
    dataset_id = H5Dopen(file_id, "/records/left", H5P_DEFAULT);
    if (dataset_id < 0) {
        goto out;
    }
    dataspace_id = H5Dget_space(dataset_id);
    if (dataspace_id < 0) {
        goto out;
    }
    rank = H5Sget_simple_extent_ndims(dataspace_id);
    if (rank != 1) {
        ret = MSP_ERR_FILE_FORMAT;
        goto out;
    }
    status = H5Sget_simple_extent_dims(dataspace_id, dims, NULL);
    if (status < 0) {
        goto out;
    }
    self->num_records = (size_t) dims[0];
    self->left = malloc(self->num_records * sizeof(uint32_t));
    self->right = malloc(self->num_records * sizeof(uint32_t));
    self->node = malloc(self->num_records * sizeof(uint32_t));
    self->children = malloc(2 * self->num_records * sizeof(uint32_t));
    self->time = malloc(self->num_records * sizeof(double));
    if (self->left == NULL || self->right == NULL || self->node == NULL
            || self->children == NULL || self->time == NULL) {
        ret = MSP_ERR_NO_MEMORY;
        goto out;
    }
    status = H5Sclose(dataspace_id);
    if (status < 0) {
        goto out;
    }
    status = H5Dclose(dataset_id);
    if (status < 0) {
        goto out;
    }
    ret = tree_sequence_check_hdf5_dimensions(self, file_id);
    if (ret != 0) {
        goto out;
    }
    ret = 0;
out:
    return ret;
}

static int
tree_sequence_read_hdf5_data(tree_sequence_t *self, hid_t file_id)
{
    herr_t status;
    int ret = MSP_ERR_HDF5;
    hid_t dataset_id;
    struct _hdf5_field_read {
        const char *name;
        hid_t type;
        void *dest;
    };
    struct _hdf5_field_read fields[] = {
        {"/breakpoints", H5T_NATIVE_UINT32, NULL},
        {"/records/left", H5T_NATIVE_UINT32, NULL},
        {"/records/right", H5T_NATIVE_UINT32, NULL},
        {"/records/node", H5T_NATIVE_UINT32, NULL},
        {"/records/children", H5T_NATIVE_UINT32, NULL},
        {"/records/time", H5T_NATIVE_DOUBLE, NULL},
    };
    size_t num_fields = sizeof(fields) / sizeof(struct _hdf5_field_read);
    size_t j;

    fields[0].dest = self->breakpoints;
    fields[1].dest = self->left;
    fields[2].dest = self->right;
    fields[3].dest = self->node;
    fields[4].dest = self->children;
    fields[5].dest = self->time;

    for (j = 0; j < num_fields; j++) {
        dataset_id = H5Dopen(file_id, fields[j].name, H5P_DEFAULT);
        if (dataset_id < 0) {
            goto out;
        }
        status = H5Dread(dataset_id, fields[j].type, H5S_ALL,
                H5S_ALL, H5P_DEFAULT, fields[j].dest);
        if (status < 0) {
            goto out;
        }
        status = H5Dclose(dataset_id);
        if (status < 0) {
            goto out;
        }
    }
    ret = 0;
out:
    return ret;
}

int
tree_sequence_load(tree_sequence_t *self, const char *filename)
{
    int ret = MSP_ERR_GENERIC;
    herr_t status;
    hid_t file_id;

    file_id = H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_id < 0) {
        ret = MSP_ERR_HDF5;
        goto out;
    }
    status = tree_sequence_read_hdf5_metadata(self, file_id);
    if (status < 0) {
        goto out;
    }
    ret = tree_sequence_read_hdf5_dimensions(self, file_id);
    if (ret != 0) {
        goto out;
    }
    ret = tree_sequence_read_hdf5_data(self, file_id);
    if (ret != 0) {
        goto out;
    }
    status = H5Fclose(file_id);
    if (status < 0) {
        ret = MSP_ERR_HDF5;
        goto out;
    }
    ret = 0;
out:
    return ret;
}

static int
tree_sequence_write_hdf5_data(tree_sequence_t *self, hid_t file_id, int flags)
{
    herr_t ret = -1;
    herr_t status;
    hid_t group_id, dataset_id, dataspace_id, plist_id;
    hsize_t dims[2];
    struct _hdf5_field_read {
        const char *name;
        hid_t storage_type;
        hid_t memory_type;
        int dimensions;
        size_t size;
        void *source;
    };
    struct _hdf5_field_read fields[] = {
        {"/breakpoints", H5T_STD_U32LE, H5T_NATIVE_UINT32, 1, 0, NULL},
        {"/records/left", H5T_STD_U32LE, H5T_NATIVE_UINT32, 1, 0, NULL},
        {"/records/right", H5T_STD_U32LE, H5T_NATIVE_UINT32, 1, 0, NULL},
        {"/records/node", H5T_STD_U32LE, H5T_NATIVE_UINT32, 1, 0, NULL},
        {"/records/children", H5T_STD_U32LE, H5T_NATIVE_UINT32, 2, 0, NULL},
        {"/records/time", H5T_IEEE_F64LE, H5T_NATIVE_DOUBLE, 1, 0, NULL},
    };
    size_t num_fields = sizeof(fields) / sizeof(struct _hdf5_field_read);
    size_t j;

    fields[0].source = self->breakpoints;
    fields[0].size = self->num_breakpoints;
    fields[1].source = self->left;
    fields[2].source = self->right;
    fields[3].source = self->node;
    fields[4].source = self->children;
    fields[5].source = self->time;
    for (j = 1; j < num_fields; j++) {
        fields[j].size = self->num_records;
    }
    /* Create the records group */
    group_id = H5Gcreate(file_id, "/records", H5P_DEFAULT, H5P_DEFAULT,
            H5P_DEFAULT);
    if (group_id < 0) {
        goto out;
    }
    status = H5Gclose(group_id);
    if (status < 0) {
        goto out;
    }
    for (j = 0; j < num_fields; j++) {
        dims[0] = fields[j].size;
        dims[1] = 2; /* unused except for children */
        dataspace_id = H5Screate_simple(fields[j].dimensions, dims, NULL);
        if (dataspace_id < 0) {
            goto out;
        }
        plist_id = H5Pcreate(H5P_DATASET_CREATE);
        if (plist_id < 0) {
            goto out;
        }
        /* Set the chunk size to the full size of the dataset since we
         * always read the full thing.
         */
        status = H5Pset_chunk(plist_id, fields[j].dimensions, dims);
        if (status < 0) {
            goto out;
        }
        if (flags & MSP_ZLIB_COMPRESSION) {
            /* Turn on byte shuffling to improve compression */
            status = H5Pset_shuffle(plist_id);
            if (status < 0) {
                goto out;
            }
            /* Set zlib compression at level 9 (best compression) */
            status = H5Pset_deflate(plist_id, 9);
            if (status < 0) {
                goto out;
            }
        }
        /* Turn on Fletcher32 checksums for integrity checks */
        status = H5Pset_fletcher32(plist_id);
        if (status < 0) {
            goto out;
        }
        dataset_id = H5Dcreate2(file_id, fields[j].name,
                fields[j].storage_type, dataspace_id, H5P_DEFAULT,
                plist_id, H5P_DEFAULT);
        if (dataset_id < 0) {
            goto out;
        }
        status = H5Dwrite(dataset_id, fields[j].memory_type, H5S_ALL,
                H5S_ALL, H5P_DEFAULT, fields[j].source);
        if (status < 0) {
            goto out;
        }
        status = H5Dclose(dataset_id);
        if (status < 0) {
            goto out;
        }
        status = H5Pclose(plist_id);
        if (status < 0) {
            goto out;
        }
    }
    ret = 0;
out:
    return ret;
}

static int
tree_sequence_write_hdf5_metadata(tree_sequence_t *self, hid_t file_id)
{
    herr_t status = -1;
    hid_t attr_id, group_id, dataspace_id;
    hsize_t dims = 1;
    uint32_t version = MSP_FILE_FORMAT_VERSION;

    struct _hdf5_metadata_write {
        const char *name;
        hid_t parent;
        hid_t storage_type;
        hid_t memory_type;
        int dimensions;
        size_t size;
        void *source;
    };
    struct _hdf5_metadata_write fields[] = {
        {"format_version", 0, H5T_STD_U32LE, H5T_NATIVE_UINT32, 1, 1, NULL},
        {"sample_size", 0, H5T_STD_U32LE, H5T_NATIVE_UINT32, 1, 1, NULL},
        {"num_loci", 0, H5T_STD_U32LE, H5T_NATIVE_UINT32, 1, 1, NULL},
    };
    /* TODO random_seed, population_models, etc. */
    size_t num_fields = sizeof(fields) / sizeof(struct _hdf5_metadata_write);
    size_t j;

    fields[0].source = &version;
    fields[1].source = &self->sample_size;
    fields[2].source = &self->num_loci;

    dataspace_id = H5Screate_simple(1, &dims, NULL);
    if (dataspace_id < 0) {
        status = dataspace_id;
        goto out;
    }
    /* make a group for the simulation parameters */
    group_id = H5Gcreate(file_id, "/parameters", H5P_DEFAULT, H5P_DEFAULT,
            H5P_DEFAULT);
    if (group_id < 0) {
        goto out;
    }
    fields[0].parent = file_id;
    for (j = 1; j < num_fields; j++) {
        fields[j].parent = group_id;
    }
    for (j = 0; j < num_fields; j++) {
        attr_id = H5Acreate(fields[j].parent, fields[j].name,
                fields[j].storage_type, dataspace_id, H5P_DEFAULT,
                H5P_DEFAULT);
        if (attr_id < 0) {
            goto out;
        }
        status = H5Awrite(attr_id, fields[j].memory_type, fields[j].source);
        if (status < 0) {
            goto out;
        }
        status = H5Aclose(attr_id);
        if (status < 0) {
            goto out;
        }
    }
    status = H5Gclose(group_id);
    if (status < 0) {
        goto out;
    }
    status = H5Sclose(dataspace_id);
    if (status < 0) {
        goto out;
    }
out:
    return status;
}

int
tree_sequence_dump(tree_sequence_t *self, const char *filename, int flags)
{
    int ret = MSP_ERR_HDF5;
    herr_t status;
    hid_t file_id;

    file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id < 0) {
        goto out;
    }
    status = tree_sequence_write_hdf5_metadata(self, file_id);
    if (status < 0) {
        goto out;
    }
    status = tree_sequence_write_hdf5_data(self, file_id, flags);
    if (status < 0) {
        goto out;
    }
    status = H5Fclose(file_id);
    if (status < 0) {
        goto out;
    }
    ret = 0;
out:
    return ret;
}


size_t
tree_sequence_get_num_breakpoints(tree_sequence_t *self)
{
    return self->num_breakpoints;
}

size_t
tree_sequence_get_num_coalescence_records(tree_sequence_t *self)
{
    return self->num_records;
}

int
tree_sequence_get_record(tree_sequence_t *self, size_t index,
        coalescence_record_t *record)
{
    int ret = MSP_ERR_OUT_OF_BOUNDS;

    if (index < self->num_records) {
        record->left = self->left[index];
        record->right = self->right[index];
        record->node = self->node[index];
        record->children[0] = self->children[2 * index];
        record->children[1] = self->children[2 * index + 1];
        record->time = self->time[index];
        ret = 0;
    }
    return ret;
}

int
tree_sequence_get_breakpoints(tree_sequence_t *self, uint32_t *breakpoints)
{
    int ret = 0;

    memcpy(breakpoints, self->breakpoints,
            self->num_breakpoints * sizeof(uint32_t));
    return ret;
}

/* ======================================================== *
 * Tree diff iterator.
 * ======================================================== */

int
tree_diff_iterator_alloc(tree_diff_iterator_t *self,
        tree_sequence_t *tree_sequence, int flags)
{
    int ret = 0;
    uint32_t n = tree_sequence->sample_size;

    self->tree_sequence = tree_sequence;
    self->flags = flags;
    self->current_left = 0;
    self->next_record_index = 0;
    /* These two are only used when we are iterating over all records */
    self->current_breakpoint_index = 0;
    self->next_breakpoint = 0;
    self->num_records = tree_sequence_get_num_coalescence_records(
            self->tree_sequence);
    /* Allocate the memory heaps */
    /* We can't have more than 2n tree_nodes used at once. */
    /* TODO This should really be 2n plus some small contstant. However,
     * we seem to hit conditions in tests where this isn't enough. As
     * a workaround for now, this is increased to 3n which is surely
     * too much. We should figure out what the real maximum is here and
     * update this.
     */
    ret = object_heap_init(&self->tree_node_heap, sizeof(tree_node_t),
            3 * n, NULL);
    if (ret != 0) {
        goto out;
    }
    /* We can have at most n active node lists */
    ret = object_heap_init(&self->tree_node_list_heap, sizeof(tree_node_list_t),
            n, NULL);
    if (ret != 0) {
        goto out;
    }
    ret = object_heap_init(&self->avl_node_heap, sizeof(avl_node_t),
            n, NULL);
    if (ret != 0) {
        goto out;
    }
    avl_init_tree(&self->active_nodes, cmp_tree_node_list, NULL);

    self->nodes_in.head = NULL;
out:
    return ret;
}

int
tree_diff_iterator_free(tree_diff_iterator_t *self)
{
    int ret = 0;

    object_heap_free(&self->tree_node_heap);
    object_heap_free(&self->tree_node_list_heap);
    object_heap_free(&self->avl_node_heap);
    return ret;
}

void
tree_diff_iterator_print_state(tree_diff_iterator_t *self)
{
    tree_node_t *tree_node;
    avl_node_t *avl_node;
    tree_node_list_t *tree_node_list;

    printf("tree_diff_iterator state\n");
    printf("current_left = %d\n", self->current_left);
    printf("next_record_index = %d\n", (int) self->next_record_index);
    printf("num_records = %d\n", (int) self->num_records);
    printf("nodes_in:\n");
    tree_node = self->nodes_in.head;
    while (tree_node != NULL) {
        printf("\t(%d\t%d)\t%d\n", tree_node->children[0],
                tree_node->children[1], tree_node->id);
        tree_node = tree_node->next;
    }
    printf("active_nodes:\n");
    for (avl_node = self->active_nodes.head; avl_node != NULL; avl_node = avl_node->next) {
        tree_node_list = (tree_node_list_t *) avl_node->item;
        printf("\t%d -> \n", tree_node_list->key);
        tree_node = tree_node_list->head;
        while (tree_node != NULL) {
            printf("\t\t(%d\t%d)\t%d\n", tree_node->children[0],
                    tree_node->children[1], tree_node->id);
            tree_node = tree_node->next;
        }
    }
    printf("Memory heaps:\n");
    printf("tree_node_heap:\n");
    object_heap_print_state(&self->tree_node_heap);
    printf("tree_node_list_heap:\n");
    object_heap_print_state(&self->tree_node_list_heap);
    printf("avl_node_heap:\n");
    object_heap_print_state(&self->avl_node_heap);
}

static inline avl_node_t * WARN_UNUSED
tree_diff_iterator_alloc_avl_node(tree_diff_iterator_t *self, uint32_t key)
{
    avl_node_t *ret = NULL;
    tree_node_list_t *l = NULL;

    if (object_heap_empty(&self->avl_node_heap)) {
        goto out;
    }
    ret = (avl_node_t *) object_heap_alloc_object(&self->avl_node_heap);
    if (ret == NULL) {
        goto out;
    }
    /* we also alloc a tree_node_list, and set it up as the value in the
     * avl node */
    if (object_heap_empty(&self->tree_node_list_heap)) {
        goto out;
    }
    l = (tree_node_list_t *) object_heap_alloc_object(
            &self->tree_node_list_heap);
    if (l == NULL) {
        goto out;
    }
    l->head = NULL;
    l->tail = NULL;
    l->key = key;
    avl_init_node(ret, l);
out:
    return ret;
}

static inline void
tree_diff_iterator_free_avl_node(tree_diff_iterator_t *self,
        avl_node_t *avl_node)
{
    tree_node_list_t *l = NULL;

    l = (tree_node_list_t *) avl_node->item;
    object_heap_free_object(&self->tree_node_list_heap, l);
    object_heap_free_object(&self->avl_node_heap, avl_node);
}

static inline tree_node_t * WARN_UNUSED
tree_diff_iterator_alloc_tree_node(tree_diff_iterator_t *self,
        coalescence_record_t *record)
{
    tree_node_t *ret = NULL;

    if (object_heap_empty(&self->tree_node_heap)) {
        goto out;
    }
    ret = (tree_node_t *) object_heap_alloc_object(
            &self->tree_node_heap);
    if (ret == NULL) {
        goto out;
    }
    ret->id = record->node;
    ret->children[0] = record->children[0];
    ret->children[1] = record->children[1];
    ret->time = record->time;
    ret->next = NULL;
out:
    return ret;
}

static inline void
tree_diff_iterator_free_tree_node(tree_diff_iterator_t *self,
        tree_node_t *node)
{
    object_heap_free_object(&self->tree_node_heap, node);
}


static int
tree_diff_iterator_process_record(tree_diff_iterator_t *self,
        coalescence_record_t *record)
{
    int ret = 0;
    tree_node_t *tree_node;
    tree_node_list_t search, *list;
    avl_node_t *avl_node;

    tree_node = tree_diff_iterator_alloc_tree_node(self, record);
    if (tree_node == NULL) {
        ret = MSP_ERR_NO_MEMORY;
        goto out;
    }
    if (self->nodes_in.head == NULL) {
        self->nodes_in.head = tree_node;
    } else {
        self->nodes_in.tail->next = tree_node;
    }
    self->nodes_in.tail = tree_node;

    /* update for the active nodes */
    search.key = record->right;
    avl_node = avl_search(&self->active_nodes, &search);
    if (avl_node == NULL) {
        avl_node = tree_diff_iterator_alloc_avl_node(self, record->right);
        if (avl_node == NULL) {
            ret = MSP_ERR_NO_MEMORY;
            goto out;
        }
        avl_node = avl_insert_node(&self->active_nodes, avl_node);
        assert(avl_node != NULL);
    }
    list = (tree_node_list_t *) avl_node->item;
    assert(list != NULL);
    tree_node = tree_diff_iterator_alloc_tree_node(self, record);
    if (tree_node == NULL) {
        ret = MSP_ERR_NO_MEMORY;
        goto out;
    }
    if (list->head == NULL) {
        list->head = tree_node;
    } else {
        list->tail->next = tree_node;
    }
    list->tail = tree_node;
out:
    return ret;
}

static int
tree_diff_iterator_next_tree(tree_diff_iterator_t *self, uint32_t *length,
        tree_node_t **nodes_out, tree_node_t **nodes_in)
{
    int ret = 0;
    coalescence_record_t cr;
    int not_done = 1;
    avl_node_t *avl_node;
    tree_node_t *tree_node, *tmp;
    tree_node_list_t search, *tree_node_list;

    *nodes_out = NULL;
    if (self->current_left != 0) {
        /* first free up the old used nodes */
        tree_node = self->nodes_in.head;
        while (tree_node != NULL) {
            tmp = tree_node->next;
            tree_diff_iterator_free_tree_node(self, tree_node);
            tree_node = tmp;
        }
        self->nodes_in.head = NULL;
        /* Set nodes_out from the active_nodes */
        search.key = self->current_left;
        avl_node = avl_search(&self->active_nodes, &search);
        assert(avl_node != NULL);
        *nodes_out = ((tree_node_list_t *) avl_node->item)->head;
        /* free up the active nodes that have just been used also */
        avl_node = avl_node->prev;
        if (avl_node != NULL) {
            assert(avl_node != NULL);
            tree_node_list = (tree_node_list_t *) avl_node->item;
            tree_node = tree_node_list->head;
            while (tree_node != NULL) {
                tmp = tree_node->next;
                tree_diff_iterator_free_tree_node(self, tree_node);
                tree_node = tmp;
            }
            avl_unlink_node(&self->active_nodes, avl_node);
            tree_diff_iterator_free_avl_node(self, avl_node);
        }
    }

    if (self->next_record_index < self->num_records) {
        while (not_done) {
            ret = tree_sequence_get_record(self->tree_sequence,
                    self->next_record_index, &cr);
            if (ret != 0) {
                goto out;
            }
            if (cr.left == self->current_left) {
                ret = tree_diff_iterator_process_record(self, &cr);
                if (ret != 0) {
                    goto out;
                }
                self->next_record_index++;
                not_done = self->next_record_index < self->num_records;
            } else {
                not_done = 0;
            }
        }
        if (self->next_record_index  == self->num_records) {
            *length = cr.right - self->current_left;
        } else {
            *length = cr.left - self->current_left;
        }
        *nodes_in = self->nodes_in.head;
        self->current_left = cr.left;
        ret = 1;
    }
out:
    return ret;
}

int
tree_diff_iterator_next(tree_diff_iterator_t *self, uint32_t *length,
        tree_node_t **nodes_out, tree_node_t **nodes_in)
{
    int ret = 0;
    uint32_t *breakpoints = self->tree_sequence->breakpoints;
    size_t num_breakpoints = self->tree_sequence->num_breakpoints;

    if (self->flags & MSP_ALL_BREAKPOINTS) {
        if (self->current_breakpoint_index < num_breakpoints - 1) {
            if (breakpoints[self->current_breakpoint_index]
                    != self->next_breakpoint) {
                *nodes_out = NULL;
                *nodes_in = NULL;
            } else {
                ret = tree_diff_iterator_next_tree(self, length, nodes_out,
                        nodes_in);
                if (ret < 0) {
                    goto out;
                }
                assert(ret == 1);
                self->next_breakpoint += *length;
            }
            self->current_breakpoint_index++;
            *length = breakpoints[self->current_breakpoint_index] -
                    breakpoints[self->current_breakpoint_index - 1];
            ret = 1;
        }
    } else {
        ret = tree_diff_iterator_next_tree(self, length, nodes_out, nodes_in);
    }
out:
    return ret;
}